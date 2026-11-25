import { expect, test } from 'vitest';
import { notableBlocks } from '@/lib/utils/notable-blocks';

test('notableBlocks lists mainnet history in height order', () => {
  const heights = notableBlocks('mainnet').map((block) => block.height);
  expect(heights[0]).toBe(0);
  expect(heights).toContain(74638);
  expect(heights).toEqual([...heights].sort((left, right) => left - right));
  expect(new Set(heights).size).toBe(heights.length);
});

test('notableBlocks offers only genesis on other networks', () => {
  expect(notableBlocks('testnet4')).toEqual([{ height: 0, label: 'Genesis' }]);
  expect(notableBlocks(undefined)).toEqual([{ height: 0, label: 'Genesis' }]);
});

test('notableBlocks omits blocks above the top', () => {
  const heights = notableBlocks('mainnet', 420000).map((block) => block.height);
  expect(heights.at(-1)).toBe(420000);
  expect(heights).not.toContain(481824);
});
