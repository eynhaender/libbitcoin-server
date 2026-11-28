import type { MiningDevice } from '@/lib/admin-contract';

// The number of devices drawn in their own color, the rest summed as other.
export const FLEET_COLORS = 8;

// The series key of the devices without a color.
export const OTHER = 'other';

// The series key of the fleet total.
export const FLEET = 'fleet';

// The color slot (0 to FLEET_COLORS - 1) of each colored device, by id.
export type FleetSlots = Readonly<Record<string, number>>;

// Devices by hash rate, highest first (id breaks ties).
function ranked(devices: readonly MiningDevice[]): MiningDevice[] {
  return [...devices].sort((a, b) => b.hashrate - a.hashrate || a.id.localeCompare(b.id));
}

// The color slots for the devices. A device keeps its slot while connected.
// A pinned device without one takes a free slot, else the slot of the
// unpinned colored device of lowest hash rate. Free slots then go to the
// uncolored devices of highest hash rate. Returns the previous object when
// nothing changed.
export function assignSlots(
  previous: FleetSlots,
  devices: readonly MiningDevice[],
  pinned: ReadonlySet<string>
): FleetSlots {
  const present = new Set(devices.map((device) => device.id));
  const slots: Record<string, number> = {};
  for (const [id, slot] of Object.entries(previous)) if (present.has(id)) slots[id] = slot;

  const order = ranked(devices);
  const free = (): number | undefined => {
    const used = new Set(Object.values(slots));
    for (let slot = 0; slot < FLEET_COLORS; slot += 1) if (!used.has(slot)) return slot;
    return undefined;
  };

  for (const device of order) {
    if (!pinned.has(device.id) || device.id in slots) continue;
    let slot = free();
    if (slot === undefined) {
      const evict = [...order].reverse().find((entry) => entry.id in slots && !pinned.has(entry.id));
      if (!evict) break;
      slot = slots[evict.id];
      delete slots[evict.id];
    }
    slots[device.id] = slot;
  }

  for (const device of order) {
    if (device.id in slots) continue;
    const slot = free();
    if (slot === undefined) break;
    slots[device.id] = slot;
  }

  const before = Object.entries(previous);
  const after = Object.entries(slots);
  const same = before.length === after.length && after.every(([id, slot]) => previous[id] === slot);
  return same ? previous : slots;
}

// Whether a device can be pinned: it has a color, or a colored device is
// not pinned (so its color can be taken).
export function canPin(id: string, slots: FleetSlots, pinned: ReadonlySet<string>): boolean {
  if (pinned.has(id) || id in slots) return true;
  const colored = Object.keys(slots);
  return colored.length < FLEET_COLORS || colored.some((entry) => !pinned.has(entry));
}

export interface FleetBand {
  // A device id, or OTHER.
  key: string;
  // The color slot, or undefined for other.
  slot?: number;
  // Hash rate at each time.
  rates: number[];
  // Current hash rate.
  current: number;
}

export interface FleetSeries {
  // Sample times (milliseconds), ascending.
  times: number[];
  // Other (when shown and any) then the shown colored devices by slot.
  bands: FleetBand[];
  // Sum of the bands at each time.
  total: number[];
  // Sum of the current hash rates of the bands.
  current: number;
  // Number of devices without a color.
  others: number;
}

// The hash rate of the shown devices on the union of their sample times
// (the minutes of their history), a device without a sample at a time
// counting zero.
export function fleetSeries(
  devices: readonly MiningDevice[],
  slots: FleetSlots,
  hidden: ReadonlySet<string>
): FleetSeries {
  const times = [...new Set(devices.flatMap((device) => device.history.map(([time]) => time)))].sort(
    (a, b) => a - b
  );
  const index = new Map(times.map((time, position) => [time, position]));
  const ratesOf = (device: MiningDevice): number[] => {
    const rates = new Array<number>(times.length).fill(0);
    for (const [time, rate] of device.history) rates[index.get(time) ?? 0] += rate;
    return rates;
  };

  const colored: FleetBand[] = [];
  const other: FleetBand = { key: OTHER, rates: new Array<number>(times.length).fill(0), current: 0 };
  let others = 0;
  for (const device of devices) {
    const slot = slots[device.id];
    if (slot === undefined) others += 1;
    if (hidden.has(device.id)) continue;
    if (slot === undefined) {
      if (hidden.has(OTHER)) continue;
      ratesOf(device).forEach((rate, position) => {
        other.rates[position] += rate;
      });
      other.current += device.hashrate;
    } else {
      colored.push({ key: device.id, slot, rates: ratesOf(device), current: device.hashrate });
    }
  }

  colored.sort((a, b) => (a.slot ?? 0) - (b.slot ?? 0));
  const bands: FleetBand[] = [];
  if (others > 0 && !hidden.has(OTHER)) bands.push(other);
  bands.push(...colored);

  const total = times.map((_, position) => bands.reduce((sum, band) => sum + band.rates[position], 0));
  const current = bands.reduce((sum, band) => sum + band.current, 0);
  return { times, bands, total, current, others };
}
