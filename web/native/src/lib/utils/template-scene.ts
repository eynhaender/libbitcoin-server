import {
  chunkRate,
  formatTemplateRate,
  ratePosition,
  rateBlend,
  templateName,
  templateRates,
  templateTiles,
  transactionRate,
  type PoolTemplates,
  type RateBlend,
} from './pool-template';
import { formatInteger } from './format';

// Templates drawn in 3D on a 2D canvas: each template is a platform, each
// transaction a box on it. The footprint is the 2D treemap tile, so area is
// virtual size; height follows the selected mode.

export type HeightMode = 'chunk' | 'own' | 'fee';

export type Rgb = readonly [number, number, number];

export interface ScenePalette {
  surface: Rgb;
  accent: Rgb;
  text: Rgb;
  muted: Rgb;
  font: string;
}

export function parseHexColor(value: string, fallback: Rgb): Rgb {
  const hex = /^#([\da-f]{3}|[\da-f]{6})$/i.exec(value.trim())?.[1];
  if (!hex) return fallback;
  const full = hex.length === 3 ? [...hex].map((digit) => digit + digit).join('') : hex;
  return [0, 2, 4].map((offset) => parseInt(full.slice(offset, offset + 2), 16)) as unknown as Rgb;
}

// The canvas counterpart of color-mix: share percent of the tint over the base.
export function mixColor(tint: Rgb, base: Rgb, share: number, shade = 1): string {
  const part = (index: number) =>
    String(Math.round(((tint[index] * share) / 100 + (base[index] * (100 - share)) / 100) * shade));
  return `rgb(${part(0)},${part(1)},${part(2)})`;
}

const rgba = (color: Rgb, alpha: number) =>
  `rgba(${String(color[0])},${String(color[1])},${String(color[2])},${String(alpha)})`;

const WIDTH = 16;
const DEPTH = 10;
const GAP = 4;
const TALLEST = 4.2;
const LOWEST = 0.12;
const PLINTH = 0.35;

// One turn of the camera per minute while spinning, in radians per millisecond.
const SPIN = (2 * Math.PI) / 60_000;

// While dropping, boxes fall in block order over the fill, each for the fall
// from the drop height, and the full block holds before it clears and refills.
const FILL = 12_000;
const FALL = 900;
const HOLD = 3_000;
const DROP = 9;

// The overview's extra gap between the projected and the mined platforms.
const SPLIT = 6;
const OVERVIEW_ELEVATION = 0.62;

interface Box {
  transaction: number;
  x0: number;
  x1: number;
  z0: number;
  z1: number;
  heights: Record<HeightMode, number>;
  blend: RateBlend;
  top: string;
  sideX: string;
  sideZ: string;
}

export interface Camera {
  azimuth: number;
  elevation: number;
  distance: number;
  x: number;
  y: number;
  z: number;
}

export interface SceneLabel {
  title: string;
  detail: string;
  extra?: string;
}

export interface SceneOptions {
  // A fixed camera over all platforms, hover and click only.
  overview?: boolean;
}

interface Drag {
  x: number;
  y: number;
  startX: number;
  startY: number;
  pan: boolean;
  moved: boolean;
}

type Face = number[];

function homeCamera(x: number): Camera {
  return { azimuth: -0.62, elevation: 0.6, distance: 22, x, y: 0.6, z: DEPTH / 2 };
}

function inside(x: number, y: number, face: Face): boolean {
  let crossing = false;
  for (let i = 0, j = face.length - 2; i < face.length; j = i, i += 2) {
    const [xi, yi, xj, yj] = [face[i], face[i + 1], face[j], face[j + 1]];
    if (yi > y !== yj > y && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) crossing = !crossing;
  }
  return crossing;
}

export class TemplateScene {
  private readonly context: CanvasRenderingContext2D;
  private readonly observer: ResizeObserver;
  private pool: PoolTemplates | null = null;
  private boxes: Box[] = [];
  private heights = new Float32Array(0);
  private order = new Uint32Array(0);
  private depths = new Float32Array(0);
  private hits: Array<{ box: number; faces: Face[] }> = [];
  private labels: SceneLabel[] = [];
  private names: SceneLabel[] | null = null;
  // The first mined platform of the overview, after the gap; -1 for none.
  private split = -1;
  private readonly overview: boolean;
  private camera = homeCamera(WIDTH / 2);
  private goal: { x: number; z: number } | null = null;
  private mode: HeightMode = 'chunk';
  private template = 0;
  private selected = -1;
  private hover = -1;
  private pointer: [number, number] | null = null;
  private drag: Drag | null = null;
  private touches = new Map<number, [number, number]>();
  private frame = 0;
  private spin = false;
  private last = 0;
  private drop = false;
  private dropped = 0;
  // Per box: its share of the fill (block order), and its lift (NaN until it falls).
  private ranks = new Float32Array(0);
  private lifts = new Float32Array(0);
  private ratio = 1;
  private palette: ScenePalette = {
    surface: [31, 33, 28],
    accent: [255, 160, 34],
    text: [237, 237, 237],
    muted: [207, 207, 207],
    font: 'sans-serif',
  };

  constructor(
    private readonly canvas: HTMLCanvasElement,
    private readonly onSelect: (transaction: number) => void,
    private readonly animate: boolean,
    options: SceneOptions = {}
  ) {
    const context = canvas.getContext('2d');
    if (!context) throw new Error('Canvas 2D context unavailable.');
    this.context = context;
    this.overview = options.overview ?? false;
    canvas.addEventListener('pointerdown', this.pointerDown);
    canvas.addEventListener('pointermove', this.pointerMove);
    canvas.addEventListener('pointerup', this.pointerUp);
    canvas.addEventListener('pointercancel', this.pointerCancel);
    canvas.addEventListener('pointerleave', this.pointerLeave);
    canvas.addEventListener('contextmenu', this.contextMenu);
    canvas.addEventListener('wheel', this.wheel, { passive: false });
    canvas.addEventListener('keydown', this.keyDown);
    this.observer = new ResizeObserver(this.resize);
    this.observer.observe(canvas);
    this.resize();
  }

  destroy(): void {
    cancelAnimationFrame(this.frame);
    this.observer.disconnect();
    this.canvas.removeEventListener('pointerdown', this.pointerDown);
    this.canvas.removeEventListener('pointermove', this.pointerMove);
    this.canvas.removeEventListener('pointerup', this.pointerUp);
    this.canvas.removeEventListener('pointercancel', this.pointerCancel);
    this.canvas.removeEventListener('pointerleave', this.pointerLeave);
    this.canvas.removeEventListener('contextmenu', this.contextMenu);
    this.canvas.removeEventListener('wheel', this.wheel);
    this.canvas.removeEventListener('keydown', this.keyDown);
  }

  setPool(pool: PoolTemplates): void {
    this.pool = pool;
    const largest = Math.max(1, ...pool.transactions.map((transaction) => transaction.fee));
    const boxes: Box[] = [];
    pool.templates.forEach((_, template) => {
      const offset = this.offset(template);
      for (const { transaction, rect } of templateTiles(pool, template, WIDTH, DEPTH)) {
        const entry = pool.transactions[transaction];
        // Keep a seam between boxes without swallowing the smallest ones.
        const seamX = Math.min(0.03, rect.width * 0.15);
        const seamZ = Math.min(0.03, rect.height * 0.15);
        const scale = (position: number) => LOWEST + TALLEST * position;
        boxes.push({
          transaction,
          x0: offset + rect.x + seamX,
          x1: offset + rect.x + rect.width - seamX,
          z0: rect.y + seamZ,
          z1: rect.y + rect.height - seamZ,
          heights: {
            chunk: scale(ratePosition(chunkRate(pool.chunks[entry.chunk]))),
            own: scale(ratePosition(transactionRate(entry))),
            fee: scale(Math.max(0, Math.log(entry.fee / 100) / Math.log(largest / 100))),
          },
          blend: rateBlend(transactionRate(entry)),
          top: '',
          sideX: '',
          sideZ: '',
        });
      }
    });

    this.boxes = boxes;
    this.heights = Float32Array.from(boxes, (box) => box.heights[this.mode]);
    this.order = Uint32Array.from(boxes, (_, index) => index);
    this.depths = new Float32Array(boxes.length);
    this.ranks = new Float32Array(boxes.length);
    const sequence = boxes.map((_, index) => index).sort((left, right) => boxes[left].transaction - boxes[right].transaction);
    sequence.forEach((box, rank) => {
      this.ranks[box] = rank / Math.max(1, sequence.length - 1);
    });
    this.lifts = new Float32Array(boxes.length);
    this.dropped = 0;
    this.labels = pool.templates.map((template, index) => ({
      title: templateName(pool, index),
      detail: `~${formatTemplateRate(templateRates(pool, template).median)} sat/vB · ${formatInteger(template.count)} txs`,
    }));
    this.hover = -1;
    this.paint();
    this.request();
  }

  // Labels in place of the template names and rates, null to restore them.
  setLabels(labels: SceneLabel[] | null): void {
    this.names = labels;
    this.request();
  }

  // Set before the pool, as box positions follow it.
  setSplit(split: number): void {
    this.split = split;
  }

  // Shift the overview by platforms, then glide it back into place, so the
  // platforms appear to move along by that many.
  slide(platforms: number): void {
    if (!this.overview || !this.animate || !this.pool) return;
    this.camera.x = this.overviewCenter() + platforms * (WIDTH + GAP);
    this.goal = { x: this.overviewCenter(), z: DEPTH / 2 };
    this.request();
  }

  setPalette(palette: ScenePalette): void {
    this.palette = palette;
    this.paint();
    this.request();
  }

  setMode(mode: HeightMode): void {
    this.mode = mode;
    if (!this.animate) this.heights = Float32Array.from(this.boxes, (box) => box.heights[mode]);
    this.request();
  }

  setSelection(template: number, transaction: number | null): void {
    if (template !== this.template) {
      this.template = template;
      this.glide(template);
    }
    this.selected = transaction ?? -1;
    this.request();
  }

  getView(): Camera {
    return { ...this.camera, ...this.goal };
  }

  setView(camera: Camera): void {
    this.camera = { ...camera };
    this.goal = null;
    this.request();
  }

  // Pans by a share of the canvas extent, positive toward the right/bottom.
  panBy(dx: number, dy: number): void {
    this.goal = null;
    this.pan(dx * this.canvas.clientWidth, dy * this.canvas.clientHeight);
    this.request();
  }

  resetView(): void {
    this.camera = homeCamera(this.center(this.template));
    this.goal = null;
    this.request();
  }

  // Orbit the camera slowly around the platform until stopped.
  setSpin(spin: boolean): void {
    this.spin = spin;
    this.last = 0;
    this.request();
  }

  // Drop the boxes in from above in block order, repeatedly, until stopped.
  setDrop(drop: boolean): void {
    this.drop = drop;
    this.dropped = 0;
    if (!drop) this.lifts.fill(0);
    this.request();
  }

  private paint(): void {
    for (const box of this.boxes) {
      const [tint, base, share] = [this.palette[box.blend.tint], this.palette[box.blend.base], box.blend.share];
      box.top = mixColor(tint, base, share);
      box.sideX = mixColor(tint, base, share, 0.62);
      box.sideZ = mixColor(tint, base, share, 0.8);
    }
  }

  private offset(template: number): number {
    return template * (WIDTH + GAP) + (this.split >= 0 && template >= this.split ? SPLIT : 0);
  }

  private center(template: number): number {
    return this.offset(template) + WIDTH / 2;
  }

  private overviewCenter(): number {
    const count = this.pool?.templates.length ?? 1;
    return (this.offset(count - 1) + WIDTH) / 2;
  }

  // Look straight at all platforms, from as far as fits them in the width.
  private fit(): void {
    const count = this.pool?.templates.length ?? 1;
    const width = this.canvas.width / this.ratio;
    const height = this.canvas.height / this.ratio;
    const half = (this.offset(count - 1) + WIDTH) / 2 + 1.2;
    const camera = this.camera;
    camera.azimuth = 0;
    camera.elevation = OVERVIEW_ELEVATION;
    camera.distance = (half * height * 1.15) / (width / 2) + DEPTH / 2 + 1;
    // Aim between the platforms and their labels, centering both.
    camera.y = -1.6;
    if (!this.goal) camera.x = this.overviewCenter();
    camera.z = DEPTH / 2;
  }

  private glide(template: number): void {
    if (this.overview) return;
    const goal = { x: this.center(template), z: DEPTH / 2 };
    if (this.animate) this.goal = goal;
    else Object.assign(this.camera, goal);
  }

  private request(): void {
    if (this.frame) return;
    this.frame = requestAnimationFrame((time) => {
      this.frame = 0;
      if (this.step(time)) this.request();
    });
  }

  // Advance animations and draw; true while anything is still moving.
  private step(time: number): boolean {
    let moving = false;
    const camera = this.camera;
    if (this.spin) {
      // Hold still under a drag, and skip the gap of a hidden tab.
      if (this.last && !this.drag)
        camera.azimuth = (camera.azimuth - Math.min(100, time - this.last) * SPIN) % (2 * Math.PI);
      this.last = time;
      moving = true;
    }

    if (this.drop) {
      if (!this.dropped) this.dropped = time;
      const phase = (time - this.dropped) % (FILL + FALL + HOLD);
      for (let index = 0; index < this.lifts.length; index += 1) {
        const fallen = (phase - this.ranks[index] * FILL) / FALL;
        this.lifts[index] = fallen < 0 ? Number.NaN : fallen >= 1 ? 0 : DROP * (1 - fallen * fallen);
      }
      moving = true;
    }

    if (this.goal) {
      camera.x += (this.goal.x - camera.x) * 0.14;
      camera.z += (this.goal.z - camera.z) * 0.14;
      if (Math.abs(this.goal.x - camera.x) + Math.abs(this.goal.z - camera.z) < 0.01) {
        Object.assign(camera, this.goal);
        this.goal = null;
      } else moving = true;
    }

    for (let index = 0; index < this.boxes.length; index += 1) {
      const target = this.boxes[index].heights[this.mode];
      const delta = target - this.heights[index];
      if (Math.abs(delta) > 0.002) {
        this.heights[index] += delta * 0.16;
        moving = true;
      } else this.heights[index] = target;
    }

    if (this.overview) this.fit();
    this.draw();
    return moving;
  }

  private readonly resize = (): void => {
    this.ratio = Math.min(2, globalThis.devicePixelRatio || 1);
    this.canvas.width = Math.max(1, Math.round(this.canvas.clientWidth * this.ratio));
    this.canvas.height = Math.max(1, Math.round(this.canvas.clientHeight * this.ratio));
    this.request();
  };

  private draw(): void {
    const { context, camera, palette, boxes, heights, lifts } = this;
    const width = this.canvas.width / this.ratio;
    const height = this.canvas.height / this.ratio;
    context.setTransform(this.ratio, 0, 0, this.ratio, 0, 0);
    context.fillStyle = rgba(palette.surface, 1);
    context.fillRect(0, 0, width, height);
    if (!this.pool) return;

    const level = Math.cos(camera.elevation);
    const ex = camera.x + camera.distance * level * Math.sin(camera.azimuth);
    const ey = camera.y + camera.distance * Math.sin(camera.elevation);
    const ez = camera.z + camera.distance * level * Math.cos(camera.azimuth);
    let [fx, fy, fz] = [camera.x - ex, camera.y - ey, camera.z - ez];
    const length = Math.hypot(fx, fy, fz);
    [fx, fy, fz] = [fx / length, fy / length, fz / length];
    const across = Math.hypot(fz, fx);
    const [rx, rz] = [-fz / across, fx / across];
    const [ux, uy, uz] = [-rz * fy, rz * fx - rx * fz, rx * fy];
    const focal = height * 1.15;
    const [cx, cy] = [width / 2, height / 2];

    const project = (x: number, y: number, z: number, out: number[], at: number): boolean => {
      const [vx, vy, vz] = [x - ex, y - ey, z - ez];
      const depth = vx * fx + vy * fy + vz * fz;
      if (depth < 0.2) return false;
      out[at] = cx + ((vx * rx + vz * rz) * focal) / depth;
      out[at + 1] = cy - ((vx * ux + vy * uy + vz * uz) * focal) / depth;
      return true;
    };
    const quad = (corners: number[]): Face | null => {
      const face = new Array<number>(8);
      for (let corner = 0; corner < 4; corner += 1)
        if (!project(corners[corner * 3], corners[corner * 3 + 1], corners[corner * 3 + 2], face, corner * 2))
          return null;
      return face;
    };
    const fill = (face: Face | null, color: string | null, stroke: string | null, line: number) => {
      if (!face) return;
      context.beginPath();
      context.moveTo(face[0], face[1]);
      for (let at = 2; at < face.length; at += 2) context.lineTo(face[at], face[at + 1]);
      context.closePath();
      if (color) {
        context.fillStyle = color;
        context.fill();
      }
      if (stroke) {
        context.strokeStyle = stroke;
        context.lineWidth = line;
        context.stroke();
      }
    };
    // Only the faces turned toward the camera are drawn.
    const prism = (
      [x0, x1, y0, y1, z0, z1]: number[],
      [top, sideX, sideZ]: string[],
      edge: string,
      faces?: Face[]
    ) => {
      const drawn: Array<[Face | null, string]> = [];
      if (ex > x1) drawn.push([quad([x1, y0, z0, x1, y1, z0, x1, y1, z1, x1, y0, z1]), sideX]);
      else if (ex < x0) drawn.push([quad([x0, y0, z0, x0, y1, z0, x0, y1, z1, x0, y0, z1]), sideX]);
      if (ez > z1) drawn.push([quad([x0, y0, z1, x1, y0, z1, x1, y1, z1, x0, y1, z1]), sideZ]);
      else if (ez < z0) drawn.push([quad([x0, y0, z0, x1, y0, z0, x1, y1, z0, x0, y1, z0]), sideZ]);
      if (ey > y1) drawn.push([quad([x0, y1, z0, x1, y1, z0, x1, y1, z1, x0, y1, z1]), top]);
      for (const [face, color] of drawn) {
        fill(face, color, edge, 0.5);
        if (face && faces) faces.push(face);
      }
    };

    const count = this.pool.templates.length;
    const end = this.offset(count - 1) + WIDTH + GAP + 4;
    const line = [0, 0, 0, 0];
    context.strokeStyle = rgba(palette.muted, 0.08);
    context.lineWidth = 1;
    const ground = -PLINTH - 0.01;
    for (let x = -8; x <= end; x += 2)
      if (project(x, ground, -8, line, 0) && project(x, ground, DEPTH + 8, line, 2)) {
        context.beginPath();
        context.moveTo(line[0], line[1]);
        context.lineTo(line[2], line[3]);
        context.stroke();
      }
    for (let z = -8; z <= DEPTH + 8; z += 2)
      if (project(-8, ground, z, line, 0) && project(end, ground, z, line, 2)) {
        context.beginPath();
        context.moveTo(line[0], line[1]);
        context.lineTo(line[2], line[3]);
        context.stroke();
      }

    // The divider between the projected and the mined platforms.
    if (this.split > 0 && this.split < count) {
      const divider = this.offset(this.split) - (GAP + SPLIT) / 2;
      const [near, far] = [[0, 0], [0, 0]];
      if (project(divider, ground, DEPTH + 2, near, 0) && project(divider, ground, -2, far, 0)) {
        context.save();
        context.setLineDash([6, 6]);
        context.strokeStyle = rgba(palette.text, 0.5);
        context.lineWidth = 1.5;
        context.beginPath();
        context.moveTo(near[0], near[1]);
        context.lineTo(far[0], far[1]);
        context.stroke();
        context.restore();
      }
    }

    const platforms = Array.from({ length: count }, (_, index) => index).sort(
      (left, right) => Math.abs(this.center(right) - ex) - Math.abs(this.center(left) - ex)
    );
    for (const template of platforms) {
      const offset = this.offset(template);
      const current = template === this.template;
      prism(
        [offset - 0.4, offset + WIDTH + 0.4, -PLINTH, 0, -0.4, DEPTH + 0.4],
        [
          mixColor(palette.muted, palette.surface, current ? 16 : 9),
          mixColor(palette.muted, palette.surface, 4, 0.8),
          mixColor(palette.muted, palette.surface, 6, 0.9),
        ],
        current ? rgba(palette.accent, 1) : rgba(palette.muted, 0.2)
      );
    }

    // Painter's order: farthest box first.
    for (let index = 0; index < boxes.length; index += 1) {
      const box = boxes[index];
      const dx = (box.x0 + box.x1) / 2 - ex;
      const dy = lifts[index] + heights[index] / 2 - ey;
      const dz = (box.z0 + box.z1) / 2 - ez;
      // A box yet to fall sorts last and is skipped.
      this.depths[index] = Number.isNaN(dy) ? -1 : dx * dx + dy * dy + dz * dz;
    }
    const depths = this.depths;
    this.order.sort((left, right) => depths[right] - depths[left]);

    const seam = rgba(palette.surface, 0.55);
    this.hits = [];
    for (const index of this.order) {
      const [box, lift] = [boxes[index], lifts[index]];
      if (Number.isNaN(lift)) continue;
      const faces: Face[] = [];
      prism([box.x0, box.x1, lift, lift + heights[index], box.z0, box.z1], [box.top, box.sideX, box.sideZ], seam, faces);
      this.hits.push({ box: index, faces });
    }

    const selected = this.pool.transactions.at(this.selected);
    if (selected) {
      const accent = rgba(palette.accent, 1);
      const text = rgba(palette.text, 1);
      for (const member of this.pool.chunks[selected.chunk].transactions) {
        const index = boxes.findIndex((box) => box.transaction === member);
        if (index < 0 || Number.isNaN(lifts[index])) continue;
        const [box, top] = [boxes[index], lifts[index] + heights[index]];
        const mine = member === this.selected;
        fill(quad([box.x0, top, box.z0, box.x1, top, box.z0, box.x1, top, box.z1, box.x0, top, box.z1]), null, mine ? accent : text, mine ? 2.5 : 1.5);
      }
    }

    context.textAlign = 'center';
    context.textBaseline = 'top';
    const span = [0, 0, 0, 0];
    (this.names ?? this.labels).forEach((label, template) => {
      if (!project(this.center(template), -PLINTH, DEPTH + 1.1, line, 0)) return;
      // At most as wide as the platform and its gap, and within the canvas.
      const fits =
        project(this.offset(template) - GAP / 2, -PLINTH, DEPTH + 1.1, span, 0) &&
        project(this.offset(template) + WIDTH + GAP / 2, -PLINTH, DEPTH + 1.1, span, 2);
      const edge = 2 * Math.max(0, Math.min(line[0], width - line[0]) - 4);
      const room = fits ? Math.min(span[2] - span[0], edge) : edge;
      context.font = `600 13px ${palette.font}`;
      context.fillStyle = rgba(template === this.template ? palette.accent : palette.muted, 1);
      context.fillText(label.title, line[0], line[1], room);
      context.font = `12px ${palette.font}`;
      context.fillStyle = rgba(palette.muted, 1);
      context.fillText(label.detail, line[0], line[1] + 17, room);
      if (label.extra) context.fillText(label.extra, line[0], line[1] + 33, room);
    });

    const hovered = boxes.at(this.hover);
    if (hovered && this.pointer) {
      const transaction = this.pool.transactions[hovered.transaction];
      const lines = [
        `${formatTemplateRate(transactionRate(transaction))} sat/vB own · ${formatTemplateRate(chunkRate(this.pool.chunks[transaction.chunk]))} chunk`,
        `${formatInteger(transaction.size)} vB · ${formatInteger(transaction.fee)} sat`,
      ];
      context.font = `12px ${palette.font}`;
      context.textAlign = 'left';
      const boxWidth = Math.max(...lines.map((text) => context.measureText(text).width)) + 20;
      const x = Math.min(this.pointer[0] + 14, width - boxWidth - 6);
      const y = Math.min(this.pointer[1] + 14, height - 50);
      context.fillStyle = rgba(palette.surface, 0.95);
      context.strokeStyle = rgba(palette.accent, 0.6);
      context.lineWidth = 1;
      context.beginPath();
      context.rect(x, y, boxWidth, 42);
      context.fill();
      context.stroke();
      context.fillStyle = rgba(palette.text, 1);
      context.fillText(lines[0], x + 10, y + 7);
      context.fillStyle = rgba(palette.muted, 1);
      context.fillText(lines[1], x + 10, y + 23);
    }
  }

  // Topmost box under the point, or -1.
  private pick(x: number, y: number): number {
    for (let index = this.hits.length - 1; index >= 0; index -= 1)
      if (this.hits[index].faces.some((face) => inside(x, y, face))) return this.hits[index].box;
    return -1;
  }

  private local(event: PointerEvent): [number, number] {
    const bounds = this.canvas.getBoundingClientRect();
    return [event.clientX - bounds.left, event.clientY - bounds.top];
  }

  private readonly pointerDown = (event: PointerEvent): void => {
    this.canvas.setPointerCapture(event.pointerId);
    const [x, y] = this.local(event);
    this.touches.set(event.pointerId, [x, y]);
    if (this.touches.size > 1) {
      if (this.drag) this.drag.moved = true;
      return;
    }
    this.drag = { x, y, startX: x, startY: y, pan: event.shiftKey || event.button === 2, moved: false };
    this.hover = -1;
    this.goal = null;
  };

  private readonly pointerMove = (event: PointerEvent): void => {
    const [x, y] = this.local(event);
    const last = this.touches.get(event.pointerId);
    if (last && this.touches.size > 1) {
      this.gesture(event.pointerId, last, [x, y]);
      return;
    }
    if (last) this.touches.set(event.pointerId, [x, y]);
    const drag = this.drag;
    if (!drag) {
      this.pointer = [x, y];
      const hover = this.pick(x, y);
      if (hover !== this.hover) {
        this.hover = hover;
        this.request();
      }
      return;
    }

    const [dx, dy] = [x - drag.x, y - drag.y];
    [drag.x, drag.y] = [x, y];
    if (Math.abs(x - drag.startX) + Math.abs(y - drag.startY) > 4) drag.moved = true;
    if (this.overview) return;
    const camera = this.camera;
    if (drag.pan) {
      this.pan(dx, dy);
    } else {
      camera.azimuth -= dx * 0.006;
      camera.elevation = Math.max(0.1, Math.min(1.45, camera.elevation + dy * 0.005));
    }
    this.request();
  };

  private readonly pointerUp = (event: PointerEvent): void => {
    if (this.release(event)) return;
    const drag = this.drag;
    this.drag = null;
    if (!drag || drag.moved) return;
    const box = this.boxes.at(this.pick(...this.local(event)));
    if (box) this.onSelect(box.transaction);
  };

  private readonly pointerCancel = (event: PointerEvent): void => {
    if (!this.release(event)) this.drag = null;
  };

  // Ends a pointer, true if others remain, the drag then following one of them.
  private release(event: PointerEvent): boolean {
    this.touches.delete(event.pointerId);
    const [rest] = this.touches.values();
    if (!rest) return false;
    const [x, y] = rest;
    this.drag = { x, y, startX: x, startY: y, pan: false, moved: true };
    return true;
  }

  // Two pointers pan by their midpoint, zoom by their spread and turn by
  // their angle, each following the fingers.
  private gesture(id: number, last: [number, number], next: [number, number]): void {
    const other = [...this.touches].find(([key]) => key !== id)?.[1];
    this.touches.set(id, next);
    if (!other) return;
    const [ox, oy] = other;
    const before = Math.hypot(last[0] - ox, last[1] - oy);
    const after = Math.hypot(next[0] - ox, next[1] - oy);
    const angle = Math.atan2(next[1] - oy, next[0] - ox) - Math.atan2(last[1] - oy, last[0] - ox);
    const turn = Math.atan2(Math.sin(angle), Math.cos(angle));
    this.pan((next[0] - last[0]) / 2, (next[1] - last[1]) / 2);
    this.camera.azimuth += turn;
    if (before > 0 && after > 0) this.zoom(before / after);
    this.request();
  }

  private readonly pointerLeave = (): void => {
    this.pointer = null;
    if (this.hover < 0) return;
    this.hover = -1;
    this.request();
  };

  private readonly contextMenu = (event: MouseEvent): void => {
    event.preventDefault();
  };

  private readonly wheel = (event: WheelEvent): void => {
    // The overview leaves the wheel to scroll the page.
    if (this.overview) return;
    event.preventDefault();
    this.zoom(Math.exp(event.deltaY * 0.0012));
  };

  private readonly keyDown = (event: KeyboardEvent): void => {
    if (this.overview) return;
    const camera = this.camera;
    switch (event.key) {
      case 'ArrowLeft': {
        camera.azimuth += 0.08;
        break;
      }
      case 'ArrowRight': {
        camera.azimuth -= 0.08;
        break;
      }
      case 'ArrowUp': {
        camera.elevation = Math.min(1.45, camera.elevation + 0.06);
        break;
      }
      case 'ArrowDown': {
        camera.elevation = Math.max(0.1, camera.elevation - 0.06);
        break;
      }
      case '+':
      case '=': {
        this.zoom(0.9);
        break;
      }
      case '-': {
        this.zoom(1.1);
        break;
      }
      default: {
        return;
      }
    }
    event.preventDefault();
    this.request();
  };

  // Moves the scene with the pointer by pixels across the canvas.
  private pan(dx: number, dy: number): void {
    const camera = this.camera;
    const scale = camera.distance * 0.0016;
    const [sin, cos] = [Math.sin(camera.azimuth), Math.cos(camera.azimuth)];
    camera.x += (-dx * cos - dy * sin) * scale;
    camera.z += (dx * sin - dy * cos) * scale;
  }

  private zoom(factor: number): void {
    this.camera.distance = Math.max(6, Math.min(110, this.camera.distance * factor));
    this.request();
  }
}
