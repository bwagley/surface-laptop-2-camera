#!/usr/bin/env python3
"""
Surface Laptop 2 webcam bridge — PipeWire-native, idle-aware
ov9734 IPU3 RAW10 (ip3G) → debayer → PipeWire Video/Source node

Creates a PipeWire Video/Source node so apps (Cheese, browsers via portal, etc.)
can find the camera. Sensor capture only starts when a real PipeWire consumer
links to our node (detected via pw-dump, not need-data which fires spuriously).
"""
import subprocess, signal, sys, os, time, threading, fcntl, select, json
import numpy as np
import gi
gi.require_version('Gst', '1.0')
from gi.repository import Gst, GLib

SENSOR_W  = 1296
SENSOR_H  = 734
STRIDE    = 1664
FRAME_IN  = STRIDE * SENSOR_H
OUT_W     = SENSOR_W // 2
OUT_H     = SENSOR_H // 2
FRAMERATE = 30
STOP_GRACE = 5.0


def find_cio2_video_node():
  out = subprocess.check_output(
      ['media-ctl', '-d', '/dev/media0', '-p'],
      text=True, stderr=subprocess.DEVNULL,
  )
  in_entity = False
  for line in out.splitlines():
      if 'ipu3-cio2 1' in line and 'entity' in line:
          in_entity = True
      if in_entity and 'device node name' in line:
          return line.split()[-1]
  raise RuntimeError('Could not find ipu3-cio2 1 video node')


def setup_media_pipeline():
  cmds = [
      ['media-ctl', '-d', '/dev/media0', '-l',
       '"ov9734 2-0036":0->"ipu3-csi2 1":0[1]'],
      ['media-ctl', '-d', '/dev/media0', '--set-v4l2',
       '"ov9734 2-0036":0[fmt:SGRBG10_1X10/1296x734]'],
      ['media-ctl', '-d', '/dev/media0', '--set-v4l2',
       '"ipu3-csi2 1":0[fmt:SGRBG10_1X10/1296x734]'],
      ['media-ctl', '-d', '/dev/media0', '--set-v4l2',
       '"ipu3-csi2 1":1[fmt:SGRBG10_1X10/1296x734]'],
      ['v4l2-ctl', '-d', '/dev/v4l-subdev7',
       '--set-ctrl=analogue_gain=80,digital_gain=400'],
  ]
  for cmd in cmds:
      subprocess.run(cmd, check=True, capture_output=True)


def unpack_ipu3_raw10(buf):
  raw = np.frombuffer(buf, dtype=np.uint8).reshape(SENSOR_H, 52, 32)
  u32 = raw.view('<u4')
  out = np.empty((SENSOR_H, 52, 25), dtype=np.uint8)
  for k in range(25):
      bit = k * 10
      w   = bit >> 5
      b   = bit & 31
      lo  = 32 - b
      if lo >= 10:
          val = (u32[:, :, w] >> b) & 0x3FF
      else:
          hi_mask = (1 << (10 - lo)) - 1
          val = ((u32[:, :, w] >> b) | ((u32[:, :, w + 1] & hi_mask) << lo)) & 0x3FF
      out[:, :, k] = (val >> 2).astype(np.uint8)
  return out.reshape(SENSOR_H, 52 * 25)[:, :SENSOR_W]


def debayer_grbg_half(raw):
  r = raw[0::2, 1::2].astype(np.uint16)
  g = (raw[0::2, 0::2].astype(np.uint16) + raw[1::2, 1::2]) // 2
  b = raw[1::2, 0::2].astype(np.uint16)
  r = np.clip(r * 2.0, 0, 255).astype(np.uint8)
  g = np.clip(g * 1.0, 0, 255).astype(np.uint8)
  b = np.clip(b * 1.8, 0, 255).astype(np.uint8)
  return np.stack([r, g, b], axis=2)


def capture_loop(appsrc, sensor_dev, stop_event):
  """Read sensor frames and push to appsrc until stop_event is set."""
  v4l2 = subprocess.Popen([
      'v4l2-ctl', '-d', sensor_dev,
      '--set-fmt-video=width=1296,height=734,pixelformat=ip3G',
      '--stream-mmap=4', '--stream-to=-', '--stream-count=0',
  ], stdout=subprocess.PIPE, stderr=subprocess.PIPE)

  fd = v4l2.stdout.fileno()
  flags = fcntl.fcntl(fd, fcntl.F_GETFL)
  fcntl.fcntl(fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

  buf = b''
  frame_count = 0
  try:
      while not stop_event.is_set():
          ready, _, _ = select.select([v4l2.stdout], [], [], 0.5)
          if not ready:
              continue
          chunk = os.read(fd, 65536)
          if not chunk:
              err = v4l2.stderr.read().decode(errors='replace').strip()
              if err:
                  print(f'v4l2-ctl error: {err}', file=sys.stderr, flush=True)
              break
          buf += chunk
          while len(buf) >= FRAME_IN:
              raw  = unpack_ipu3_raw10(buf[:FRAME_IN])
              buf  = buf[FRAME_IN:]
              rgb  = debayer_grbg_half(raw)
              frame_count += 1
              if frame_count % 30 == 0:
                  print(f'  {frame_count} frames', flush=True)
              gst_buf = Gst.Buffer.new_wrapped(rgb.tobytes())
              appsrc.emit('push-buffer', gst_buf)
  finally:
      v4l2.terminate()
      v4l2.wait()


def main():
  Gst.init(None)

  try:
      sensor_dev = find_cio2_video_node()
  except (RuntimeError, subprocess.CalledProcessError) as e:
      print(f'Device discovery failed: {e}', file=sys.stderr)
      sys.exit(1)
  print(f'Sensor: {sensor_dev}', flush=True)

  try:
      setup_media_pipeline()
  except subprocess.CalledProcessError as e:
      print(f'media-ctl setup failed: {e}', file=sys.stderr)
      sys.exit(1)

  caps_str = (f'video/x-raw,format=RGB,'
              f'width={OUT_W},height={OUT_H},'
              f'framerate={FRAMERATE}/1')
  pipeline = Gst.parse_launch(
      f'appsrc name=src is-live=true block=false caps="{caps_str}" ! '
      f'videoconvert ! '
      f'pipewiresink name=pw-sink sync=false'
  )
  appsrc   = pipeline.get_by_name('src')
  pw_sink  = pipeline.get_by_name('pw-sink')

  # Configure as a Video/Source node so WirePlumber lists it under Sources
  stream_props = Gst.Structure.new_from_string(
      'props,'
      'media.class=(string)Video/Source,'
      'node.name=(string)ov9734-webcam,'
      'node.description=(string)"Surface Laptop 2 Webcam"'
  )
  pw_sink.set_property('stream-properties', stream_props)

  lock           = threading.Lock()
  capture_thread = [None]
  stop_event     = [None]
  last_had_consumers = [0.0]

  def start_capture():
      with lock:
          if capture_thread[0] and capture_thread[0].is_alive():
              return
          print('Consumer connected — starting sensor capture', flush=True)
          stop_event[0] = threading.Event()
          t = threading.Thread(
              target=capture_loop,
              args=(appsrc, sensor_dev, stop_event[0]),
              daemon=True,
          )
          capture_thread[0] = t
          t.start()

  def stop_capture():
      with lock:
          ev = stop_event[0]
          t  = capture_thread[0]
      if ev:
          ev.set()
      if t:
          t.join(timeout=3)
      with lock:
          capture_thread[0] = None
          stop_event[0]     = None
      print('No consumers — sensor off', flush=True)

  pipeline.set_state(Gst.State.PLAYING)
  print('Idle — PipeWire Video/Source active, sensor off', flush=True)

  glib_loop = GLib.MainLoop()

  def cleanup(sig=None, frame=None):
      print('\nShutting down...', flush=True)
      with lock:
          ev = stop_event[0]
      if ev:
          ev.set()
      t = capture_thread[0]
      if t:
          t.join(timeout=3)
      pipeline.set_state(Gst.State.NULL)
      glib_loop.quit()

  signal.signal(signal.SIGTERM, cleanup)
  signal.signal(signal.SIGINT,  cleanup)

  def get_consumer_count(node_id):
      """Return number of active PipeWire links where we are the source."""
      try:
          data = json.loads(subprocess.check_output(
              ['pw-dump'], text=True, stderr=subprocess.DEVNULL, timeout=3,
          ))
          links = [o for o in data if o.get('type') == 'PipeWire:Interface:Link']
          return sum(
              1 for l in links
              if l.get('info', {}).get('output-node-id') == node_id
          )
      except Exception:
          return 0

  def find_our_node_id():
      """Poll pw-dump until our PipeWire node appears (may take a moment)."""
      deadline = time.time() + 15
      while time.time() < deadline:
          try:
              data = json.loads(subprocess.check_output(
                  ['pw-dump'], text=True, stderr=subprocess.DEVNULL, timeout=3,
              ))
              for obj in data:
                  if obj.get('type') == 'PipeWire:Interface:Node':
                      props = obj.get('info', {}).get('props', {})
                      if props.get('node.name') == 'ov9734-webcam':
                          return obj['id']
          except Exception:
              pass
          time.sleep(0.5)
      return None

  def pw_watcher():
      """Detect real PipeWire consumers via pw-dump links; start/stop capture."""
      node_id = find_our_node_id()
      if node_id is None:
          print('Warning: PW node not found — consumer detection disabled', file=sys.stderr, flush=True)
          return
      print(f'PW node ID: {node_id}', flush=True)

      while glib_loop.is_running():
          time.sleep(1)
          n = get_consumer_count(node_id)
          now = time.time()
          with lock:
              running = capture_thread[0] and capture_thread[0].is_alive()
          if n > 0:
              last_had_consumers[0] = now
              if not running:
                  start_capture()
          else:
              if running and (now - last_had_consumers[0]) > STOP_GRACE:
                  stop_capture()

  threading.Thread(target=pw_watcher, daemon=True).start()
  glib_loop.run()


if __name__ == '__main__':
  main()
