import { expect, test } from 'vitest';
import { assemblySlots, projectSlices } from '@/lib/utils/assembly-line';

const fees = (histogram: Array<[number, number]>) => ({
  count: 0,
  virtual: histogram.reduce((sum, [, size]) => sum + size, 0),
  fee: 0,
  histogram,
});

test('assembly_line__slots__site_width__two_and_two', () => {
  expect(assemblySlots(1248)).toEqual({ projected: 2, mined: 2 });
});

test('assembly_line__slots__phone_width__one_and_one', () => {
  expect(assemblySlots(343)).toEqual({ projected: 1, mined: 1 });
});

test('assembly_line__slots__odd__extra_mined', () => {
  expect(assemblySlots(950)).toEqual({ projected: 1, mined: 2 });
});

test('assembly_line__slots__wide__capped', () => {
  expect(assemblySlots(10_000)).toEqual({ projected: 4, mined: 4 });
});

test('assembly_line__project__empty_pool__no_blocks', () => {
  expect(projectSlices(undefined, 0, 4)).toEqual([]);
});

test('assembly_line__project__bins_straddle__split_by_virtual', () => {
  const slices = projectSlices(fees([[10, 600_000], [5, 600_000], [2, 300_000]]), 0, 4);
  expect(slices).toHaveLength(2);
  expect(slices[0].elements).toEqual([
    { virtual: 600_000, fee: 6_000_000, cluster: 0 },
    { virtual: 400_000, fee: 2_000_000, cluster: 1 },
  ]);
  expect(slices[1].elements).toEqual([
    { virtual: 200_000, fee: 1_000_000, cluster: 0 },
    { virtual: 300_000, fee: 600_000, cluster: 1 },
  ]);
});

test('assembly_line__project__deep_pool__last_holds_rest', () => {
  const slices = projectSlices(fees([[20, 1_000_000], [10, 1_000_000], [5, 1_000_000], [1, 500_000]]), 0, 2);
  expect(slices.map((slice) => slice.blocks)).toEqual([1, 3]);
  expect(slices[1].elements).toEqual([{ virtual: 1_000_000, fee: 10_000_000, cluster: 0 }]);
});

test('assembly_line__project__skip_held__later_blocks_only', () => {
  const slices = projectSlices(fees([[20, 1_000_000], [10, 1_000_000], [5, 1_000_000]]), 1, 1);
  expect(slices).toEqual([{ blocks: 2, elements: [{ virtual: 1_000_000, fee: 10_000_000, cluster: 0 }] }]);
});

test('assembly_line__project__next_block_only__not_rest', () => {
  const slices = projectSlices(fees([[20, 1_000_000], [10, 3_000_000]]), 0, 1);
  expect(slices).toEqual([{ blocks: 1, elements: [{ virtual: 1_000_000, fee: 20_000_000, cluster: 0 }] }]);
});
