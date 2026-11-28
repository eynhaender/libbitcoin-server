import { useMemo, useRef, useState, useSyncExternalStore } from 'react';
import { FitHash } from '@/components/fit-hash';
import { FleetChart, PinIcon, slotColor } from '@/components/fleet-chart';
import { HashrateChart } from '@/components/hashrate-chart';
import { FLEET_COLORS, OTHER, assignSlots, canPin, fleetSeries, type FleetSlots } from '@/lib/mining/fleet';
import type { AdminMiningFrame, MiningDevice } from '@/lib/admin-contract';
import { useCurrentTime } from '@/lib/use-current-time';
import { formatInteger } from '@/lib/format';
import type { WorkerMetricsStore } from '@/lib/worker-metrics-store';
import {
  formatDifficulty,
  formatDuration,
  formatHashrate,
  formatShortRelativeTime,
} from '@/lib/mining/format';

// The node's mining console: the channels of its stratum services, by
// service, the detail of the one chosen, and the blocks they found.
export function Mining({ metrics }: { metrics: WorkerMetricsStore }) {
  const mining = useSyncExternalStore(metrics.subscribeMining, metrics.getMining, metrics.getMining);
  const [chosen, setChosen] = useState<string>();
  const [pinned, setPinned] = useState<ReadonlySet<string>>(new Set());
  const [hidden, setHidden] = useState<ReadonlySet<string>>(new Set());
  const [focus, setFocus] = useState<string | null>(null);
  const now = useCurrentTime();
  const devices = useMemo(() => mining?.devices ?? [], [mining]);
  const device = devices.find((entry) => entry.id === chosen) ?? devices[0];
  const fleet = devices.reduce((sum, entry) => sum + entry.hashrate, 0);
  const services = [...new Set(devices.map((entry) => entry.service))];

  // Slots persist across frames, so a device keeps its color while connected.
  const previous = useRef<FleetSlots>({});
  const slots = useMemo(() => {
    previous.current = assignSlots(previous.current, devices, pinned);
    return previous.current;
  }, [devices, pinned]);
  const series = useMemo(() => fleetSeries(devices, slots, hidden), [devices, slots, hidden]);
  const names = useMemo(() => new Map(devices.map((entry) => [entry.id, deviceName(entry)])), [devices]);
  const colors = Object.keys(slots).length;
  const toggle = (set: ReadonlySet<string>, key: string) => {
    const next = new Set(set);
    if (!next.delete(key)) next.add(key);
    return next;
  };

  return (
    <div className="space-y-8">
      <header className="flex flex-wrap items-end justify-between gap-4">
        <div>
          <h1 className="text-accent text-2xl font-semibold">Mining</h1>
          <p className="text-textMuted text-sm">The channels of the node&apos;s stratum services.</p>
        </div>
        <p className="text-textMuted text-sm">
          Fleet <strong className="text-textPrimary text-lg tabular-nums">{formatHashrate(fleet)}</strong> ·{' '}
          {formatInteger(devices.length)} active
        </p>
      </header>

      {devices.length === 0 ? (
        <p className="border-accent/30 bg-surface/80 text-textMuted rounded-2xl border p-5 text-sm">
          {mining ? 'No miners connected.' : 'Waiting for the mining feed…'}
        </p>
      ) : (
        <>
          <section
            aria-labelledby="fleet-hashrate-title"
            className="border-accent/30 bg-surface/80 space-y-2 rounded-2xl border p-5"
          >
            <div>
              <h2 id="fleet-hashrate-title" className="text-accent text-lg font-semibold">
                Fleet hashrate
              </h2>
              <p className="text-textMuted text-sm">
                {colors < devices.length
                  ? `The ${String(FLEET_COLORS)} largest or pinned devices in color, the other ${String(devices.length - colors)} summed.`
                  : 'Each device in color, stacked to the fleet.'}
              </p>
            </div>
            <FleetChart
              series={series}
              slots={slots}
              names={names}
              pinned={pinned}
              hidden={hidden}
              focus={focus}
              onFocus={setFocus}
              onToggle={(key) => {
                setHidden((current) => toggle(current, key));
              }}
            />
          </section>

          <div className="flex flex-wrap items-start gap-6">
            <nav
              aria-label="Devices"
              className="border-accent/30 bg-surface/80 flex min-w-0 flex-[1_1_380px] flex-col gap-1 rounded-2xl border p-2"
            >
              {services.map((service) => (
                <div key={service} className="flex flex-col gap-1">
                  <p className="text-accentSecondary px-3 pt-2 pb-1 text-xs tracking-wide uppercase">{service}</p>
                  {devices
                    .filter((entry) => entry.service === service)
                    .map((entry) => {
                      const slot = slots[entry.id];
                      const key = slot === undefined ? OTHER : entry.id;
                      const isPinned = pinned.has(entry.id);
                      return (
                        <DeviceRow
                          key={entry.id}
                          dot={deviceDot(entry, mining)}
                          color={slot === undefined ? 'var(--theme-text-muted)' : slotColor(slot)}
                          current={device?.id === entry.id}
                          dimmed={hidden.has(entry.id) || (focus !== null && focus !== key)}
                          onClick={() => {
                            setChosen(entry.id);
                          }}
                          onFocus={(on) => {
                            setFocus(on ? key : null);
                          }}
                          name={deviceName(entry)}
                          detail={`${entry.client || 'unknown client'} · ${entry.transport}${slot === undefined ? ' · in other' : ''}`}
                          rate={formatHashrate(entry.hashrate)}
                          pinned={isPinned}
                          pinnable={canPin(entry.id, slots, pinned)}
                          onPin={() => {
                            setPinned((current) => toggle(current, entry.id));
                          }}
                          visible={!hidden.has(entry.id)}
                          onHide={() => {
                            setHidden((current) => toggle(current, entry.id));
                          }}
                        />
                      );
                    })}
                </div>
              ))}
            </nav>

            <section className="min-w-0 flex-[999_1_560px] space-y-4">
              {device && <DeviceDetail device={device} now={now} />}
            </section>
          </div>
        </>
      )}

      {mining && mining.blocks.length > 0 && <Blocks mining={mining} now={now} />}
    </div>
  );
}

// The color of a share result: accepted blue, submitted green, an invalid
// share (a fault) red, and any other rejection amber.
function resultColor(result: string): string {
  if (result === 'accepted') return 'text-sky-300';
  if (result === 'submitted') return 'text-emerald-300';
  return result === 'invalid' ? 'text-rose-300' : 'text-amber-300';
}

// The status of a device, green once it has submitted a block, else as its
// most recent share.
function deviceDot(device: MiningDevice, mining: AdminMiningFrame | null): string {
  if (mining?.blocks.some((block) => block.device === device.id)) return 'bg-emerald-300';
  const result = device.shares[0]?.result ?? 'accepted';
  if (result === 'accepted' || result === 'submitted') return 'bg-sky-300';
  return result === 'invalid' ? 'bg-rose-300' : 'bg-amber-300';
}

// The workers of a device, or its identifier without any.
function deviceName(device: MiningDevice): string {
  return device.workers.length > 0 ? device.workers.join(', ') : device.id;
}

function EyeIcon({ open }: { open: boolean }) {
  return (
    <svg
      width={16}
      height={16}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth={2}
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden
    >
      {open ? (
        <>
          <path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12z" />
          <circle cx="12" cy="12" r="3" />
        </>
      ) : (
        <>
          <path d="M3 3l18 18" />
          <path d="M10.6 5.1A10.9 10.9 0 0 1 12 5c6.5 0 10 7 10 7a17.6 17.6 0 0 1-3.2 4.2" />
          <path d="M6.6 6.6C3.9 8.3 2 12 2 12s3.5 7 10 7a9.7 9.7 0 0 0 5.4-1.6" />
        </>
      )}
    </svg>
  );
}

// A device of the list: its chart color, status, detail and rate (choosing
// it), and buttons to pin its color and to hide it from the fleet chart.
function DeviceRow({
  dot,
  color,
  current,
  dimmed,
  onClick,
  onFocus,
  name,
  detail,
  rate,
  pinned,
  pinnable,
  onPin,
  visible,
  onHide,
}: {
  dot: string;
  color: string;
  current: boolean;
  dimmed: boolean;
  onClick: () => void;
  onFocus: (on: boolean) => void;
  name: string;
  detail: string;
  rate: string;
  pinned: boolean;
  pinnable: boolean;
  onPin: () => void;
  visible: boolean;
  onHide: () => void;
}) {
  const icon = 'flex h-8 w-8 shrink-0 items-center justify-center rounded-lg border transition';
  return (
    <div
      onMouseEnter={() => {
        onFocus(true);
      }}
      onMouseLeave={() => {
        onFocus(false);
      }}
      className={`flex items-center gap-2 rounded-xl border py-1 pr-1.5 pl-2 transition ${
        current ? 'border-accent/50 bg-accentSecondary/20' : 'hover:bg-accentSecondary/10 border-transparent'
      } ${dimmed ? 'opacity-50' : ''}`}
    >
      <span className="h-9 w-1 shrink-0 rounded-sm" style={{ background: color }} aria-hidden />
      <button
        type="button"
        onClick={onClick}
        aria-current={current ? 'true' : undefined}
        className="flex min-w-0 flex-1 items-center gap-3 py-1.5 text-left"
      >
        <span className={`h-2.5 w-2.5 shrink-0 rounded-full ${dot}`} aria-hidden />
        <span className="min-w-0 flex-1">
          <span className="text-textPrimary block truncate font-semibold">{name}</span>
          <span className="text-textMuted block truncate text-xs">{detail}</span>
        </span>
        <span className="text-textPrimary text-sm font-medium whitespace-nowrap tabular-nums">{rate}</span>
      </button>
      <button
        type="button"
        onClick={onPin}
        disabled={!pinnable}
        aria-pressed={pinned}
        aria-label={pinned ? `Unpin ${name}` : `Pin ${name} in color`}
        title={pinnable ? (pinned ? 'Unpin' : 'Pin in color') : 'All colors are pinned'}
        className={`${icon} disabled:opacity-30 ${
          pinned ? 'border-accent/50 bg-accent/15 text-accent' : 'text-textMuted hover:text-accent border-transparent'
        }`}
      >
        <PinIcon />
      </button>
      <button
        type="button"
        onClick={onHide}
        aria-pressed={!visible}
        aria-label={visible ? `Hide ${name} from the fleet chart` : `Show ${name} in the fleet chart`}
        title={visible ? 'Hide from the fleet chart' : 'Show in the fleet chart'}
        className={`${icon} text-textMuted hover:text-accent border-transparent`}
      >
        <EyeIcon open={visible} />
      </button>
    </div>
  );
}

function Stat({ label, value }: { label: string; value: string }) {
  return (
    <div>
      <dt className="text-accentSecondary text-xs tracking-wide uppercase">{label}</dt>
      <dd className="text-textPrimary mt-0.5 text-xl font-semibold tabular-nums">{value}</dd>
    </div>
  );
}

function DeviceDetail({ device, now }: { device: MiningDevice; now: number }) {
  const history = device.history.map(([time, rate]) => ({ time, rate }));
  const rejections = Object.entries(device.rejections);
  return (
    <>
      <div className="border-accent/30 bg-surface/80 space-y-5 rounded-2xl border p-5">
        <div className="min-w-0">
          <h3 className="text-accent truncate text-xl font-semibold">{deviceName(device)}</h3>
          <p className="text-textMuted text-sm">
            {device.service} · {device.client || 'unknown client'} · {device.transport} ·{' '}
            <span className="font-mono">{device.address}</span> · connected{' '}
            {formatDuration((now - device.connected) / 1000)} ago
          </p>
        </div>
        <dl className="grid grid-cols-2 gap-4 sm:grid-cols-3 lg:grid-cols-5">
          <Stat label="Hashrate" value={formatHashrate(device.hashrate)} />
          <Stat label="Difficulty" value={formatDifficulty(device.difficulty)} />
          <Stat label="Accepted" value={formatInteger(device.accepted)} />
          <Stat label="Rejected" value={formatInteger(device.rejected)} />
          <Stat
            label="Last share"
            value={device.last_share === null ? '—' : formatShortRelativeTime(device.last_share / 1000, now)}
          />
        </dl>
        {rejections.length > 0 && (
          <p className="text-textMuted text-xs">
            Rejected by reason:{' '}
            {rejections.map(([reason, count]) => `${reason} ${formatInteger(count)}`).join(' · ')}
          </p>
        )}
        <HashrateChart history={history} span={1800} />
      </div>

      <div className="border-accent/30 bg-surface/80 rounded-2xl border p-5">
        <h3 className="text-accent mb-3 text-lg font-semibold">Recent shares</h3>
        {device.shares.length === 0 ? (
          <p className="text-textMuted text-sm">No share submitted yet.</p>
        ) : (
          <div className="overflow-x-auto">
            <table className="w-full text-sm tabular-nums">
              <thead className="text-textMuted text-left">
                <tr>
                  <th className="py-1 pr-4 font-normal">Found</th>
                  <th className="py-1 pr-4 font-normal">Template</th>
                  <th className="py-1 pr-4 text-right font-normal">Difficulty</th>
                  <th className="py-1 font-normal">Result</th>
                </tr>
              </thead>
              <tbody>
                {device.shares.map((share, index) => (
                  <tr key={`${String(share.time)}-${String(index)}`} className="border-accent/10 border-t">
                    <td className="py-1 pr-4 whitespace-nowrap">{formatShortRelativeTime(share.time / 1000, now)}</td>
                    <td className="py-1 pr-4">{share.height === 0 ? '—' : formatInteger(share.height)}</td>
                    <td className="py-1 pr-4 text-right">{formatDifficulty(share.difficulty)}</td>
                    <td className="py-1">
                      <span className={resultColor(share.result)}>
                        {share.result === 'accepted'
                          ? 'Accepted'
                          : share.result === 'submitted'
                            ? 'Submitted'
                            : `Rejected: ${share.result}`}
                      </span>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </>
  );
}

function Blocks({ mining, now }: { mining: AdminMiningFrame; now: number }) {
  const names = new Map(mining.devices.map((device) => [device.id, deviceName(device)]));
  return (
    <section className="border-accent/30 bg-surface/80 rounded-2xl border p-5">
      <h3 className="text-accent mb-3 text-lg font-semibold">Blocks found</h3>
      <div className="overflow-x-auto">
        <table className="w-full table-fixed text-sm tabular-nums">
          <colgroup>
            <col className="w-24" />
            <col className="w-28" />
            <col />
            <col className="w-40" />
          </colgroup>
          <thead className="text-textMuted text-left">
            <tr>
              <th className="py-1 pr-4 font-normal">Height</th>
              <th className="py-1 pr-4 font-normal">Found</th>
              <th className="py-1 pr-4 font-normal">Hash</th>
              <th className="py-1 font-normal">Device</th>
            </tr>
          </thead>
          <tbody>
            {mining.blocks.map((block) => (
              <tr key={block.hash} className="border-accent/10 border-t">
                <td className="py-1 pr-4">{formatInteger(block.height)}</td>
                <td className="py-1 pr-4 whitespace-nowrap">{formatShortRelativeTime(block.time / 1000, now)}</td>
                <td data-fit className="overflow-hidden py-1 pr-4 font-mono whitespace-nowrap">
                  <FitHash value={block.hash} />
                </td>
                <td className="truncate py-1">{names.get(block.device) ?? block.device}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    </section>
  );
}
