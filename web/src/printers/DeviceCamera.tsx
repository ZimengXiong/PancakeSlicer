import { useEffect, useRef, useState } from 'react';
import { responseError } from '../api/access';
import { Icon } from '../components/Icon';
import { Glyph } from '../components/Glyph';

export interface Camera { id: string; name: string; format: 'mjpeg' | 'mp4'; url: string }

function MP4Camera({ url, onError, onRatio }: { url: string; onError(message: string): void; onRatio(ratio: number): void }) {
  const video = useRef<HTMLVideoElement>(null);
  useEffect(() => {
    const abort = new AbortController();
    const media = new MediaSource(), objectURL = URL.createObjectURL(media);
    const element = video.current!;
    element.src = objectURL;
    const run = async () => {
      try {
        const response = await fetch(url, { signal: abort.signal });
        if (!response.ok) throw await responseError(response);
        const reader = response.body!.getReader();
        let init = new Uint8Array(0), codec = '';
        while (!codec) {
          const chunk = await reader.read();
          if (chunk.done) throw new Error('The camera stream disconnected before video could start.');
          const joined = new Uint8Array(init.length + chunk.value.length); joined.set(init); joined.set(chunk.value, init.length); init = joined;
          if (init.length > 2 << 20) throw new Error('The camera sent an unexpectedly large video header.');
          for (let i = 4; i + 8 < init.length; i++) {
            if (init[i] === 97 && init[i + 1] === 118 && init[i + 2] === 99 && init[i + 3] === 67) {
              codec = `avc1.${[...init.slice(i + 5, i + 8)].map((v) => v.toString(16).padStart(2, '0')).join('')}`; break;
            }
          }
        }
        const mime = `video/mp4; codecs="${codec}"`;
        if (!MediaSource.isTypeSupported(mime)) throw new Error('Your browser or device doesn\'t support decoding this camera\'s H.264 video stream.');
        const buffer = media.addSourceBuffer(mime);
        const append = (data: Uint8Array) => new Promise<void>((resolve, reject) => {
          const done = () => { cleanup(); resolve(); }, failed = () => { cleanup(); reject(new Error('Oops! Couldn\'t decode the live video stream from the camera.')); };
          const cleanup = () => { buffer.removeEventListener('updateend', done); buffer.removeEventListener('error', failed); abort.signal.removeEventListener('abort', failed); };
          buffer.addEventListener('updateend', done, { once: true }); buffer.addEventListener('error', failed, { once: true }); abort.signal.addEventListener('abort', failed, { once: true });
          try { buffer.appendBuffer(data as Uint8Array<ArrayBuffer>); } catch (error) { cleanup(); reject(error); }
        });
        await append(init);
        while (!abort.signal.aborted) {
          const { done, value } = await reader.read();
          if (done) throw new Error('Camera stream disconnected');
          await append(value);
          if (buffer.buffered.length) {
            const end = buffer.buffered.end(buffer.buffered.length - 1);
            if (end - element.currentTime > 3) element.currentTime = Math.max(buffer.buffered.start(0), end - 0.5);
            if (end > 20 && buffer.buffered.start(0) < end - 20) {
              await new Promise<void>((resolve) => { buffer.addEventListener('updateend', () => resolve(), { once: true }); buffer.remove(0, end - 15); });
            }
          }
        }
      } catch (error) { if (!abort.signal.aborted) onError(error instanceof Error ? error.message : 'Camera unavailable'); }
    };
    media.addEventListener('sourceopen', () => void run(), { once: true });
    return () => { abort.abort(); element.removeAttribute('src'); element.load(); URL.revokeObjectURL(objectURL); };
  }, [url, onError]);
  return <video ref={video} autoPlay muted playsInline aria-label="Live printer camera" onResize={(e) => { const v = e.currentTarget; if (v.videoWidth && v.videoHeight) onRatio(v.videoWidth / v.videoHeight); }} />;
}

function PrinterStill({ cover }: { cover?: string }) {
  const [failed, fail] = useState<string>();
  return cover && failed !== cover ? <img className="device-camera-still" src={cover} alt="" onError={() => fail(cover)} /> : null;
}

export function DeviceCamera({ printer, cover, light, lightLabel, cameraLabel, busy, onLight }: { printer: string; cover?: string; light?: boolean; lightLabel: string; cameraLabel?: string; busy: boolean; onLight(): void }) {
  const frame = useRef<HTMLElement>(null);
  const [ratio, setRatio] = useState(16 / 9);
  const [fullscreenError, setFullscreenError] = useState('');
  const fullscreen = () => {
    const action = document.fullscreenElement ? document.exitFullscreen() : frame.current?.requestFullscreen();
    void action?.catch(() => setFullscreenError('Fullscreen isn\'t available right now'));
  };
  const [cameras, setCameras] = useState<Camera[]>([]), [selected, select] = useState('');
  const [error, setError] = useState(''), [watching, watch] = useState(true), [attempt, retry] = useState(0), [loading, setLoading] = useState(true);
  useEffect(() => {
    const abort = new AbortController();
    setLoading(true); setCameras([]); setError(''); select('');
    void fetch(`/api/printer-connections/${encodeURIComponent(printer)}/camera`, { signal: abort.signal }).then(async (r) => {
      if (!r.ok) throw await responseError(r);
      const result = await r.json() as { cameras: Camera[] };
      if (!abort.signal.aborted) { setCameras(result.cameras); select(result.cameras[0]?.id ?? ''); }
    }).catch((e: unknown) => { if (!abort.signal.aborted) setError(e instanceof Error ? e.message : 'Camera unavailable'); }).finally(() => { if (!abort.signal.aborted) setLoading(false); });
    return () => abort.abort();
  }, [printer, attempt]);
  const camera = cameras.find((x) => x.id === selected);
  if (error || (!loading && !camera)) {
    const why = error.replace(/^Camera unavailable\.?\s*/i, '');
    return <section className="device-camera device-camera-off" aria-label="Camera"><PrinterStill cover={cover} />
      {error && <div className="device-camera-why" role="status"><strong>Camera unavailable</strong>{why && <span>{why}</span>}
        <button type="button" className="btn" onClick={() => retry(attempt + 1)}>Reconnect</button></div>}</section>;
  }
  return <section ref={frame} className={`device-camera ${watching ? '' : 'camera-hidden'}`} aria-label="Camera">
    {!watching ? <div className="device-camera-hidden"><span>Camera hidden</span><button type="button" className="btn" onClick={() => watch(true)}>Show camera</button></div>
      : <><div className="device-camera-frame" style={{ aspectRatio: ratio }}>
        {loading ? <p role="status">Connecting camera…</p>
          : !camera ? <p>No LAN camera set up</p>
          : camera.format === 'mjpeg' ? <img key={`${camera.url}:${attempt}`} src={camera.url} alt="Live printer camera" onLoad={(e) => { const img = e.currentTarget; if (img.naturalWidth && img.naturalHeight) setRatio(img.naturalWidth / img.naturalHeight); }} onError={() => setError('Couldn\'t connect to the camera. Double-check your printer\'s access code and make sure LAN Liveview is turned on.')} />
          : <MP4Camera key={`${camera.url}:${attempt}`} url={camera.url} onError={setError} onRatio={setRatio} />}
      </div><div className="device-camera-toolbar">
        <span className="device-camera-label">{cameraLabel ?? camera?.name ?? 'Camera'}</span>
        {cameras.length > 1 && <select aria-label="Camera" value={selected} onChange={(e) => { select(e.target.value); setError(''); }}>{cameras.map((x) => <option key={x.id} value={x.id}>{x.name}</option>)}</select>}
        {light != null && <button type="button" aria-label={lightLabel} aria-pressed={light} disabled={busy} onClick={onLight}><Icon name={light ? 'monitor_lamp_on' : 'monitor_lamp_off'} size={18} /></button>}
        {camera && <button type="button" aria-label="Fullscreen camera" onClick={fullscreen}><Glyph name="fullscreen" size={18} /></button>}
        {camera && <button type="button" aria-label="Hide camera" onClick={() => watch(false)}><Glyph name="minus" size={18} /></button>}
      </div>{fullscreenError && <span className="device-camera-note" role="status">{fullscreenError}</span>}</>}
  </section>;
}
