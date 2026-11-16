import { useState } from 'react';
import { SKINS, type Skin, currentSkin, setSkin } from '../../../shared/skins';

export function SkinSelect() {
  const [skin, setSelected] = useState(currentSkin);

  return (
    <select
      value={skin}
      onChange={(event) => {
        const next = event.target.value as Skin;
        setSkin(next);
        setSelected(next);
      }}
      className="border-accent/40 bg-surface/80 text-textPrimary hover:border-accent focus:border-accent rounded-md border py-1 pr-8 pl-2 text-xs tracking-wide uppercase focus:ring-0 focus:outline-none"
      aria-label="Skin"
      title="Switch skin"
    >
      {SKINS.map((option) => (
        <option key={option.id} value={option.id}>
          {option.label}
        </option>
      ))}
    </select>
  );
}
