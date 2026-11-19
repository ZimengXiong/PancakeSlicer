const ios = typeof navigator !== 'undefined' && (/iPhone|iPad|iPod/.test(navigator.userAgent) || (/Macintosh/.test(navigator.userAgent) && navigator.maxTouchPoints > 1));

export function fileAccept(exts: string[]): string {
  return ios ? '' : exts.flatMap((e) => [`.${e}`, `.${e.toUpperCase()}`]).join(',');
}
