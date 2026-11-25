import { useEffect, useMemo, useRef, useState } from 'react';
import { Link, useNavigate } from 'react-router-dom';
import {
  useFees,
  useMinedBlocks,
  useRecentMiners,
  useTemplate,
  useTemplateNotifications,
} from '@/lib/hooks/useBlockchainData';
import { useNow } from '@/lib/hooks/useNow';
import { useTheme } from '@/lib/theme';
import { assemblySlots, projectSlices } from '@/lib/utils/assembly-line';
import { formatInteger, formatRelativeTime } from '@/lib/utils/format';
import { formatBitcoin, formatTemplateRate, templateRates } from '@/lib/utils/pool-template';
import { toPoolTemplates, type TemplateElement } from '@/lib/utils/pool-templates';
import { blockByHashPath, templatePath } from '@/lib/utils/routes';
import { TemplateScene, type SceneLabel } from '@/lib/utils/template-scene';
import { readPalette } from './template-scene-view';

const feesPath = '/v1/fees';

// About ten minutes a block.
function inMinutes(blocks: number): string {
  const minutes = blocks * 10;
  return minutes < 120 ? `in ~${String(minutes)} min` : `in ~${String(Math.round(minutes / 60))} h`;
}

interface Platform {
  elements: readonly TemplateElement[];
  title: string;
  detail: (median: number, count: number) => string;
  extra: string;
  path: string;
  // The newest mined block, to tell when the line moves.
  hash?: string;
}

// The blocks projected from the pool and the latest mined, as platforms of
// their txs on one line, projected left of the divider and mined right of it.
export function AssemblyLine() {
  const frame = useRef<HTMLElement>(null);
  const canvas = useRef<HTMLCanvasElement>(null);
  const scene = useRef<TemplateScene | null>(null);
  const [width, setWidth] = useState(1200);
  const slots = useMemo(() => assemblySlots(width), [width]);
  const navigate = useNavigate();
  const theme = useTheme();
  const now = useNow(30_000);

  const { data: fees } = useFees();
  const { data: mined } = useMinedBlocks(slots.mined);
  const { data: miners } = useRecentMiners(slots.mined);
  const top = mined?.at(0)?.summary.height;
  const next = (top ?? 0) + 1;
  const { data: template } = useTemplate(next, { enabled: top !== undefined });
  useTemplateNotifications();

  // Left to right: the furthest projected block to the next, then the mined
  // blocks from the newest.
  const platforms = useMemo((): Platform[] => {
    if (top === undefined) return [];
    const held = template && template.length > 0 ? template : undefined;
    const later = projectSlices(fees, held ? 1 : 0, slots.projected - (held ? 1 : 0));
    let ahead = held ? 1 : 0;
    const projected: Platform[] = [];
    if (held)
      projected.push({
        elements: held,
        title: `#${formatInteger(next)}`,
        detail: (median, count) => `~${formatTemplateRate(median)} sat/vB · ${formatInteger(count)} txs`,
        extra: `${formatBitcoin(held.reduce((sum, element) => sum + element.fee, 0))} · ${inMinutes(1)}`,
        path: templatePath(next),
      });
    later.forEach((slice, index) => {
      ahead += slice.blocks;
      const height = next + projected.length;
      projected.push({
        elements: slice.elements,
        title: slice.blocks > 1 ? `+${String(slice.blocks)} blocks` : `#${formatInteger(height)}`,
        detail: (median) => `~${formatTemplateRate(median)} sat/vB · estimate`,
        extra: `~${formatBitcoin(slice.elements.reduce((sum, element) => sum + element.fee, 0))} · ${inMinutes(ahead)}`,
        path: index === 0 && !held ? templatePath(next) : feesPath,
      });
    });

    const chain = (mined ?? []).map(({ summary, stats, transactions }): Platform => {
      const miner = miners?.miners.at(miners.top - summary.height);
      const fee = stats.feesSats;
      return {
        elements: transactions,
        title: `#${formatInteger(summary.height)}`,
        detail: (median, count) => `~${formatTemplateRate(median)} sat/vB · ${formatInteger(count)} txs`,
        extra: [
          fee === undefined ? undefined : formatBitcoin(fee),
          formatRelativeTime(summary.timestamp, now),
          miner === undefined ? undefined : (miner?.name ?? 'Unknown'),
        ]
          .filter(Boolean)
          .join(' · '),
        path: blockByHashPath(summary.hash),
        hash: summary.hash,
      };
    });
    return [...projected.reverse(), ...chain];
  }, [top, next, template, fees, mined, miners, slots.projected, now]);

  const split = platforms.findIndex((platform) => platform.hash !== undefined);
  const newest = split < 0 ? undefined : platforms[split].hash;
  const pool = useMemo(() => toPoolTemplates(platforms.map((platform) => platform.elements)), [platforms]);
  const labels = useMemo(
    () =>
      platforms.map((platform, index): SceneLabel => {
        const block = pool.templates[index];
        return {
          title: platform.title,
          detail: platform.detail(templateRates(pool, block).median, block.count),
          extra: platform.extra,
        };
      }),
    [platforms, pool]
  );

  const paths = useRef<{ platforms: Platform[]; pool: typeof pool }>({ platforms, pool });
  paths.current = { platforms, pool };

  useEffect(() => {
    const element = frame.current;
    if (!element) return;
    const observer = new ResizeObserver(() => {
      setWidth(element.clientWidth);
    });
    observer.observe(element);
    return () => {
      observer.disconnect();
    };
  }, []);

  useEffect(() => {
    if (!canvas.current) return;
    const animate = !globalThis.matchMedia('(prefers-reduced-motion: reduce)').matches;
    const created = new TemplateScene(
      canvas.current,
      (transaction) => {
        const { platforms: current, pool: shown } = paths.current;
        const path = current.at(shown.transactions[transaction]?.template ?? -1)?.path;
        if (path) void navigate(path);
      },
      animate,
      { overview: true }
    );
    scene.current = created;
    return () => {
      created.destroy();
      scene.current = null;
    };
  }, [navigate]);

  useEffect(() => {
    scene.current?.setPalette(readPalette());
  }, [theme]);

  // The line moves along by one platform when a new block is mined.
  const shown = useRef<string | undefined>(undefined);
  useEffect(() => {
    const current = scene.current;
    if (!current || pool.templates.length === 0) return;
    current.setSplit(split);
    current.setPool(pool);
    current.setSelection(Math.max(0, split - 1), null);
    if (shown.current !== undefined && newest !== undefined && newest !== shown.current) current.slide(1);
    shown.current = newest;
  }, [pool, split, newest]);

  useEffect(() => {
    scene.current?.setLabels(labels);
  }, [labels]);

  return (
    <section ref={frame} aria-label="Assembly line of projected and mined blocks">
      <canvas
        ref={canvas}
        aria-hidden="true"
        className="bg-surface block w-full rounded-xl"
        style={{ height: `${String(Math.round(Math.min(320, Math.max(190, width * 0.1 + 100))))}px` }}
      />
      <ul className="sr-only">
        {platforms.map((platform, index) => (
          <li key={`${platform.title}-${String(index)}`}>
            <Link to={platform.path}>
              {platform.hash ? 'Mined' : 'Projected'} block {platform.title}: {labels[index]?.detail},{' '}
              {platform.extra}
            </Link>
          </li>
        ))}
      </ul>
    </section>
  );
}
