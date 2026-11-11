import { APP_VERSION, GITHUB_URL, reloadBuildInfo, shortSha, useBuildInfo, type BuildInfo } from '../build-info';
import { Modal, ModalBody, ModalFooter, ModalTitle } from './Modal';
import { Sep } from '../separator';

const link = (href: string, text: string) => <a className="link" href={href} target="_blank" rel="noreferrer">{text}</a>;

export function AboutContent({ info }: { info: BuildInfo }) {
  const { about } = info;
  // This version's source when the service names one, else the repository (AGPL-3.0 §13).
  const source = (about?.source_configured && about.source_url) || GITHUB_URL;
  return <dl className="modal-grid">
    {about && <><dt>Build</dt><dd data-truncated="true" data-full-text={about.revision}>{shortSha(about.revision)}</dd></>}
    {info.engine && <><dt>Engine</dt><dd data-truncated="true" data-full-text={info.engineSha}>{info.engine}{info.engineSha ? ` ${shortSha(info.engineSha)}` : ''}</dd></>}
    {about?.image && <><dt>Image</dt><dd>{about.image}</dd></>}
    <dt>Links</dt><dd className="about-links">{link(GITHUB_URL, 'GitHub')}</dd>
    <dt>Licence</dt><dd>{link('https://www.gnu.org/licenses/agpl-3.0.html', about?.license || 'AGPL-3.0')}<Sep/>{link('/third-party-licences.txt', 'Third-party licences')}</dd>
    {source && <><dt>Source</dt><dd>{link(source, source.replace(/^https?:\/\//, '').replace(/[0-9a-f]{40}/, (sha) => sha.slice(0, 7)))}</dd></>}
  </dl>;
}

export default function About({ close }: { close: () => void }) {
  const info = useBuildInfo();
  return <Modal size="m" label="About PancakeSlicer" onClose={close}>
    <ModalTitle title="About PancakeSlicer" onClose={close} />
    <ModalBody>
      <div className="about-head"><img src="/logo.svg" width={56} height={56} alt="" /><div><strong>PancakeSlicer</strong><span>v{APP_VERSION}</span></div></div>
      <AboutContent info={info} />
      {info.state === 'loading' && <p role="status">Loading…</p>}
      {info.state === 'failed' && <p role="alert">Oops! Couldn't load build info from the slicing service.</p>}
    </ModalBody>
    <ModalFooter>
      {info.state === 'failed' && <button type="button" className="btn" onClick={reloadBuildInfo}>Retry</button>}
      <button type="button" className="btn-primary" onClick={close}>Close</button>
    </ModalFooter>
  </Modal>;
}
