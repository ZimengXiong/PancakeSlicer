import { useEffect, useState } from 'react';
import { Tooltip } from 'radix-ui';
const NAV_KEYS = new Set(['Tab', 'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Home', 'End', 'PageUp', 'PageDown', 'Enter', ' ', 'Escape', 'F6']);
const CONTROL = 'button,a,[role=button],[role=tab],[role=menuitem]';
const nameOf = (node: HTMLElement) => node.innerText.trim() ? undefined : node.getAttribute('aria-label')?.trim() || node.textContent?.trim() || undefined;
const FULL = '[data-full-text],[data-truncated]';
const cutText = (node: HTMLElement) => node.dataset.truncated === 'true' || [node, ...node.querySelectorAll<HTMLElement>('*')].some((n) => n.scrollWidth > n.clientWidth + 1 && getComputedStyle(n).textOverflow === 'ellipsis')
  ? node.dataset.fullText || node.textContent?.trim() || undefined : undefined;
const DELAY = 250, SKIP = 300, LONG_PRESS = 500;
export function ControlHints() {
  const [hint, setHint] = useState<{text: string; x: number; y: number} | null>(null);
  useEffect(() => {
    let target: HTMLElement | null = null, timer: ReturnType<typeof setTimeout> | undefined;
    const untitle = (el: Element) => {
      const text = el.getAttribute('title')?.trim();
      el.removeAttribute('title');
      if (text && el instanceof HTMLElement && !el.getAttribute('aria-label') && !el.getAttribute('aria-labelledby') && !el.textContent?.trim()) el.setAttribute('aria-label', text);
    };
    const sweep = (root: Element) => { if (root.hasAttribute('title')) untitle(root); root.querySelectorAll('[title]').forEach(untitle); };
    const titles = new MutationObserver((records) => { for (const r of records) { if (r.type === 'attributes') { if (r.target instanceof Element && r.target.hasAttribute('title')) untitle(r.target); } else r.addedNodes.forEach((n) => { if (n instanceof Element) sweep(n); }); } });
    const root = document.documentElement;
    const pointerInput = () => { root.dataset.input = 'pointer'; };
    const keyInput = (event: KeyboardEvent) => { if (NAV_KEYS.has(event.key) && !event.metaKey && !event.ctrlKey) root.dataset.input = 'keyboard'; };
    document.addEventListener('pointerdown', pointerInput, true); document.addEventListener('keydown', keyInput, true);
    sweep(document.body); titles.observe(document.body, {subtree: true, childList: true, attributes: true, attributeFilter: ['title']});
    let hiddenAt = 0, shown = false, press: ReturnType<typeof setTimeout> | undefined;
    const clear = () => { clearTimeout(timer); clearTimeout(press); if (shown) hiddenAt = Date.now(); shown = false; setHint(null); target = null; };
    const open = (node: HTMLElement | null, delay: number) => {
      if (node === target) return;
      clear();
      if (!node) return;
      target = node;
      timer = setTimeout(() => {
        if (target !== node || !node.isConnected) return;
        const text = node.matches(FULL) ? cutText(node) : nameOf(node);
        if (!text) return;
        const r = node.getBoundingClientRect(); shown = true; setHint({text, x: r.left + r.width / 2, y: r.bottom});
      }, Date.now() - hiddenAt < SKIP ? 0 : delay);
    };
    const control = (event: Event) => event.target instanceof Element ? event.target.closest<HTMLElement>(FULL) ?? event.target.closest<HTMLElement>(CONTROL) : null;
    const show = (event: PointerEvent) => { if (event.pointerType === 'mouse') open(control(event), DELAY); };
    const focus = (event: FocusEvent) => { if (root.dataset.input === 'keyboard') open(control(event), 0); };
    const touch = (event: PointerEvent) => { if (event.pointerType === 'mouse') { clear(); return; } const node = control(event); clear(); press = setTimeout(() => open(node, 0), LONG_PRESS); };
    const release = () => clearTimeout(press);
    const leave = (event: PointerEvent) => { if (event.pointerType !== 'mouse' || target?.contains(event.relatedTarget as Node | null)) return; clear(); };
    const escape = (event: KeyboardEvent) => { if (event.key === 'Escape') clear(); };
    document.addEventListener('pointerover', show); document.addEventListener('pointerout', leave); document.addEventListener('focusin', focus); document.addEventListener('focusout', clear);
    document.addEventListener('pointerdown', touch, true); document.addEventListener('pointerup', release, true); document.addEventListener('pointercancel', clear, true); document.addEventListener('scroll', clear, true); document.addEventListener('keydown', escape);
    document.addEventListener('click', clear, true);
    return () => { document.removeEventListener('pointerdown', pointerInput, true); document.removeEventListener('keydown', keyInput, true); delete root.dataset.input; titles.disconnect(); clearTimeout(timer); document.removeEventListener('pointerover', show); document.removeEventListener('pointerout', leave); document.removeEventListener('focusin', focus); document.removeEventListener('focusout', clear); document.removeEventListener('pointerdown', touch, true); document.removeEventListener('pointerup', release, true); document.removeEventListener('pointercancel', clear, true); document.removeEventListener('scroll', clear, true); document.removeEventListener('keydown', escape); document.removeEventListener('click', clear, true); };
  }, []);
  return <Tooltip.Provider><Tooltip.Root open={!!hint}><Tooltip.Trigger asChild><span aria-hidden className="hint-anchor" style={{left: hint?.x ?? 0, top: hint?.y ?? 0}}/></Tooltip.Trigger><Tooltip.Portal><Tooltip.Content aria-hidden className="control-tooltip" side="bottom" sideOffset={6} collisionPadding={12}>{hint?.text}</Tooltip.Content></Tooltip.Portal></Tooltip.Root></Tooltip.Provider>;
}
