import { Skeleton } from './Skeleton';

export function SettingsSkeleton({ tabs = true }: { tabs?: boolean }) {
  return <div className="settings-skeleton" role="status" aria-label="Loading settings">
    {tabs && <div className="settings-skeleton-tabs">{[56, 64, 52, 60].map((w, i) => <Skeleton key={i} width={w} height={20}/>)}</div>}
    {[0, 1, 2, 3, 4, 5].map((i) => <div key={i} className="opt-line"><span className="opt-label"><Skeleton width={`${70 - i * 7}%`} height={10}/></span>
      <span className="opt-field"><Skeleton block height={26} style={{ width: '100%' }}/></span></div>)}
  </div>;
}
