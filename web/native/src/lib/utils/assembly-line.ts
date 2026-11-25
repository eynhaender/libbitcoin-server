import type { PoolFees } from '@/lib/api/client';
import { BLOCK_VIRTUAL_SIZE, type TemplateElement } from './pool-templates';

// The assembly line: the blocks projected from the pool on the left, the
// blocks mined on the right, each a platform of about SLOT_PIXELS width.

export const SLOT_PIXELS = 300;

export interface AssemblySlots {
  projected: number;
  mined: number;
}

// Slots that fit the width, at least one of each and the odd one mined.
export function assemblySlots(width: number): AssemblySlots {
  const count = Math.min(8, Math.max(2, Math.floor(width / SLOT_PIXELS)));
  const projected = Math.floor(count / 2);
  return { projected, mined: count - projected };
}

export interface ProjectedSlice {
  // The blocks this one stands for, more than one only for the rest of the pool.
  blocks: number;
  // The bins (one element each) of the first block it stands for.
  elements: TemplateElement[];
}

// Cut the pool (histogram by descending rate) into blocks, skipping those
// already shown, the last of the count standing for the rest of the pool
// (unless it is the next block). The pool has no txs beyond the held
// template, so each bin is one element.
export function projectSlices(fees: PoolFees | undefined, skip: number, count: number): ProjectedSlice[] {
  const blocks: TemplateElement[][] = [];
  let current: TemplateElement[] = [];
  let filled = 0;
  for (const [rate, size] of fees?.histogram ?? []) {
    let left = size;
    while (left > 0) {
      const part = Math.min(left, BLOCK_VIRTUAL_SIZE - filled);
      current.push({ virtual: part, fee: Math.round(rate * part), cluster: current.length });
      filled += part;
      left -= part;
      if (filled >= BLOCK_VIRTUAL_SIZE) {
        blocks.push(current);
        current = [];
        filled = 0;
      }
    }
  }
  if (current.length > 0) blocks.push(current);

  const shown = blocks.slice(skip);
  return shown.slice(0, count).map((elements, index) => ({
    blocks: index === count - 1 && skip + index > 0 ? shown.length - index : 1,
    elements,
  }));
}
