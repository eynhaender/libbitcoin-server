// Skins shared by the admin dashboard and the explorer, after libbitcoin.org.
// Classic is the base palette; skins.css overrides it per :root[data-skin].

export const SKINS = [
  { id: 'moon', label: 'Moon' },
  { id: 'spec', label: 'Spec' },
  { id: 'classic', label: 'Classic' },
] as const;

export type Skin = (typeof SKINS)[number]['id'];

const DEFAULT: Skin = 'classic';
const STORAGE_KEY = 'skin';

function isSkin(value: string | null): value is Skin {
  return SKINS.some((skin) => skin.id === value);
}

// A skin given in the query (?skin=name) is remembered; ?skin=off forgets it.
function initialSkin(): Skin {
  let skin = new URLSearchParams(window.location.search).get('skin');
  try {
    if (skin === 'off') localStorage.removeItem(STORAGE_KEY);
    else if (skin === null) skin = localStorage.getItem(STORAGE_KEY);
    else localStorage.setItem(STORAGE_KEY, skin);
  } catch {
    // Storage may be unavailable; the skin then lasts for this page only.
  }
  return isSkin(skin) ? skin : DEFAULT;
}

function show(skin: Skin) {
  document.documentElement.dataset.skin = skin;
  const background = getComputedStyle(document.documentElement)
    .getPropertyValue('--color-background')
    .trim();
  if (background) {
    document.querySelector('meta[name="theme-color"]')?.setAttribute('content', background);
  }
}

export function setSkin(skin: Skin) {
  try {
    localStorage.setItem(STORAGE_KEY, skin);
  } catch {
    // Storage may be unavailable; the skin then lasts for this page only.
  }
  show(skin);
}

export function currentSkin(): Skin {
  const skin = document.documentElement.dataset.skin ?? null;
  return isSkin(skin) ? skin : DEFAULT;
}

export function initSkin() {
  show(initialSkin());
}
