const scrollKeys = ['ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'PageUp', 'PageDown', 'Home', 'End', ' '];
let pressed: EventTarget | null = null;
addEventListener('pointerdown', (e) => { pressed = e.target; }, true);

function scrolls(e: KeyboardEvent) {
  if (!scrollKeys.includes(e.key)) return false;
  const horizontal = e.key === 'ArrowLeft' || e.key === 'ArrowRight';
  const from = e.target === document.body ? pressed : e.target;
  for (let el = from instanceof Element ? from : null; el && el !== document.body; el = el.parentElement) {
    const { overflowX, overflowY } = getComputedStyle(el);
    if (horizontal
      ? /auto|scroll/.test(overflowX) && el.scrollWidth > el.clientWidth
      : /auto|scroll/.test(overflowY) && el.scrollHeight > el.clientHeight) return true;
  }
  return false;
}

export const ownsKeys = (e: KeyboardEvent) =>
  e.target instanceof Element && !!e.target.closest('input,select,textarea,[contenteditable="true"],[role="slider"],[role="menu"],[role="listbox"],[aria-haspopup="listbox"],[role="dialog"],dialog') || scrolls(e);

export function menuKeys(e: { key: string; preventDefault(): void; stopPropagation(): void }, menu: HTMLElement | null, close: () => void, back?: HTMLElement | null): boolean {
  if (!menu) return false;
  const items = [...menu.querySelectorAll<HTMLButtonElement>('.menu-item:not(:disabled)')];
  const at = items.indexOf(document.activeElement as HTMLButtonElement);
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') items[(at + (e.key === 'ArrowDown' ? 1 : -1) + items.length) % items.length]?.focus();
  else if (e.key === 'Home' || e.key === 'End') items[e.key === 'Home' ? 0 : items.length - 1]?.focus();
  else if (e.key === 'Escape') {
    close();
    back?.focus();
  } else return false;
  e.preventDefault();
  e.stopPropagation();
  return true;
}
