// The stage: the kiln mouth in the dark, drawn by one WebGL1 fragment shader (a pointed brick arch in a wall lit by the
// fire, a ray-cast brick chamber with flames, the floor, the haze and embers). The mouth is also the Play button
// (Mouth in AppShell.tsx): this loop springs it between its Play size and the pilot light in the corner and moves the
// button with it, so the hit area always sits on the drawn arch. When WebGL is missing, fails to compile or loses its
// context, or the fire is turned off (Settings, Appearance; KILN_LAUNCHER_NO_WEBGL=1), .k-app keeps no data-gl and
// base.css draws the CSS mouth instead.
// Budget: at most 1000 px wide (0.72 of the window at 1280), ~30 fps on Play (15 where WebKit composites nothing:
// flatDrawing), ~9 fps behind a sheet, full rate only while the mouth moves or the heat changes; behind a sheet, once
// the mouth is the pilot light, only the header band (stageCanvas: the rest is under the sheet, and where WebKitGTK
// composites nothing each frame repainted the whole window and the page with it, inbox 94); nothing while the window is
// hidden, and a still frame while the game runs (the GPU is the game's then; the loop then looks four times a second instead of every frame). With reduced motion a still
// frame, drawn again only when what it shows changes (wakeStage), and no flicker.
import { useEffect, useRef, type RefObject } from "react";

export type Heat = "unlit" | "idle" | "flare" | "embers" | "low";

/** Written by the shell as it renders and on pointer moves; read by the loop each frame. */
export const stageInput = { play: true, heat: "idle" as Heat, hover: false, game: false, mx: 0, my: 0 };

/** How hot the fire burns for each state (1: the hot idle on hover). */
function target(i: typeof stageInput): number {
  switch (i.heat) {
    case "flare": return 1.75;
    case "embers": return 0.85;
    case "low": return 0.45;
    case "unlit": return i.play ? 0.1 : 0.22;
    default: return i.hover ? 0.98 : i.play ? 0.64 : 0.8;
  }
}

export const VERT = "attribute vec2 p;void main(){gl_Position=vec4(p,0.,1.);}";

// uR canvas size; uT time; uH heat; uF flicker; uD mouth (centre x, bottom y, half width, height) in canvas px;
// uM pointer (-1..1); uS canvas px per CSS px; uL floor height (CSS px); uK 0 on Play, 1 as the pilot light; uO the
// canvas rows below this canvas (the header band behind a sheet: stageCanvas), so it draws the whole picture's top rows.
export const FRAG = [
  "#ifdef GL_FRAGMENT_PRECISION_HIGH",
  "precision highp float;",
  "#else",
  "precision mediump float;",
  "#endif",
  "uniform vec2 uR;uniform float uT;uniform float uH;uniform float uF;uniform vec4 uD;uniform vec2 uM;uniform float uS;uniform float uL;uniform float uK;uniform float uO;",
  "const float AA=.32;const float APEX=1.2806;",
  "float hash(vec2 p){p=fract(p*vec2(123.34,456.21));p+=dot(p,p+45.32);return fract(p.x*p.y);}",
  "float vn(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.-2.*f);return mix(mix(hash(i),hash(i+vec2(1.,0.)),f.x),mix(hash(i+vec2(0.,1.)),hash(i+vec2(1.,1.)),f.x),f.y);}",
  "float fbm(vec2 p){float v=0.,a=.5;for(int i=0;i<4;i++){v+=a*vn(p);p=p*2.03+vec2(1.7,9.2);a*=.5;}return v;}",
  // the pointed arch: two circular arcs (centres AA outside the far jambs) over straight jambs, half width 1
  "float sdArch(vec2 p,float H){float yc=H-APEX;p.x=abs(p.x);if(p.y>yc)return length(p-vec2(-AA,yc))-(1.+AA);float dx=p.x-1.;if(p.y<0.)return length(vec2(max(dx,0.),p.y));return dx;}",
  "vec4 brick(vec2 uv,vec2 s){vec2 c=uv/s;float r=floor(c.y);c.x+=.5*mod(r,2.);vec2 id=floor(c);vec2 f=fract(c);vec2 e=min(f,1.-f)*s;float m=smoothstep(.02*s.y,.07*s.y,min(e.x,e.y));return vec4(m,hash(id+vec2(r*.37,3.1)),f);}",
  "vec3 ramp(float f){vec3 c=vec3(.05,.012,.006);c=mix(c,vec3(.55,.08,.015),smoothstep(.05,.45,f));c=mix(c,vec3(1.,.38,.05),smoothstep(.35,.8,f));c=mix(c,vec3(1.,.73,.25),smoothstep(.75,1.15,f));c=mix(c,vec3(1.,.96,.8),smoothstep(1.1,1.55,f));return c;}",
  "float flame(vec2 p,float s){float tt=uT*(1.+.12*s);vec2 q=vec2(p.x*1.5+s*3.7,p.y*1.05-tt*1.3);float a=fbm(q);float b=fbm(q*1.85+vec2(a*1.3,-tt*.55));float top=(.75+1.05*uH)*exp(-p.x*p.x*.45);float f=(top-p.y)/max(top,.25);f=f*1.45+(b-.5)*1.9;return clamp(f,0.,1.7)*step(-.05,p.y);}",
  // inside the mouth: a barrel-vaulted brick chamber seen from a camera that follows the pointer a little
  "vec3 chamber(vec2 q,float H){vec2 qq=q;qq.x+=sin(q.y*8.+uT*3.1)*.008*uH;",
  "vec3 ro=vec3(uM.x*.34,.72+uM.y*.12,-2.4);vec3 rd=normalize(vec3(qq,0.)-ro);",
  "float Yc=.55,Rc=1.75,Zb=6.;float a=rd.x*rd.x+rd.y*rd.y;float b=2.*(ro.x*rd.x+(ro.y-Yc)*rd.y);float c=ro.x*ro.x+(ro.y-Yc)*(ro.y-Yc)-Rc*Rc;float disc=max(b*b-4.*a*c,0.);",
  "float tc=(-b+sqrt(disc))/(2.*a+.0001);float tf=rd.y<0.?-ro.y/rd.y:1000.;float tb=(Zb-ro.z)/rd.z;float t=min(tc,min(tf,tb));vec3 p=ro+rd*t;vec3 n;vec2 uv;",
  "if(t==tf){n=vec3(0.,1.,0.);uv=p.xz;}else if(t==tb){n=vec3(0.,0.,-1.);uv=p.xy;}else{n=normalize(vec3(-p.x,-(p.y-Yc),0.));uv=vec2(atan(p.x,p.y-Yc)*Rc,p.z);}",
  "vec4 br=brick(uv,vec2(.62,.27));vec3 alb=mix(vec3(.30,.13,.08),vec3(.46,.22,.13),br.y)*(.35+.65*br.x);",
  "vec3 L=vec3(0.,.75,2.6);vec3 l=L-p;float dl=length(l);l/=dl;float diff=max(dot(n,l),0.)*.85+.15;float F=uH*(.72+.28*uF);",
  "vec3 col=alb*diff*vec3(1.,.5,.2)*F*3.2/(1.+.28*dl*dl);col+=vec3(.9,.28,.05)*exp(-dl*.9)*F*.35;",
  "if(t==tf){float dz=(p.z-2.6)*.9;float cz=exp(-dz*dz)*exp(-p.x*p.x*.45);float cn=vn(p.xz*7.+vec2(0.,uT*.12));col+=vec3(1.,.3,.04)*cn*cn*cn*cz*2.4*F;}",
  "float fl=0.;for(int i=0;i<3;i++){float z=2.+float(i)*.6;float tp=(z-ro.z)/rd.z;if(tp<t){vec3 fp=ro+rd*tp;fl+=flame(fp.xy,float(i))*.36;}}",
  // a cold kiln (no game, Kiln not installed) shows no flames, only the last glow
  "col=mix(col,ramp(fl),smoothstep(.06,.42,fl)*smoothstep(.12,.34,uH));",
  "vec2 s=vec2(q.x*9.,q.y*9.-uT*2.6);vec2 sid=floor(s);float sp=step(.972,hash(sid))*(1.-smoothstep(0.,.34,length(fract(s)-.5)))*(1.-smoothstep(.4,H,q.y));col+=vec3(1.,.46,.13)*sp*min(uH*1.2,1.2)*smoothstep(.12,.34,uH);",
  "return col;}",
  "float embers(vec2 w,float sc,float sp,float seed){vec2 p=w/sc;p.y-=uT*sp;p.x+=sin(p.y*.7+seed)*.35;vec2 id=floor(p);float h=hash(id+seed);if(h<.965)return 0.;vec2 f=fract(p)-.5;f+=vec2(hash(id+1.3),hash(id+2.7))*.6-.3;float tw=.6+.4*sin(uT*3.+h*60.);return tw*(1.-smoothstep(0.,.05,length(f)));}",
  "void main(){vec2 px=gl_FragCoord.xy+vec2(0.,uO);vec2 w=px/uS;float hw=uD.z;float Hd=uD.w/hw;vec2 q=(px-uD.xy)/hw;float d=sdArch(q,Hd);float F=uH*(.72+.28*uF);",
  "vec2 lc=uD.xy+vec2(0.,uD.w*.35);float dist=length((px-lc)/hw);vec3 col;",
  "if(d<0.){col=chamber(q,Hd);col*=1.-.6*smoothstep(-.2,0.,d);}",
  // the floor in front of the mouth (the strip under the facts), lit in a pool; gone as the mouth becomes the pilot light
  "else if(w.y<uL){float dep=1.-w.y/uL;vec2 g=vec2((px.x-uD.x)/uS/(1.+dep*1.6),w.y*3.);vec4 fb=brick(g,vec2(170.,54.));vec3 st=mix(vec3(.10,.065,.05),vec3(.19,.12,.085),fb.y)*(.45+.55*fb.x);float pool=exp(-abs(q.x)*.62)*exp(-dep*1.3);col=st*pool*vec3(1.,.5,.22)*F*3.*(1.-uK);col+=vec3(1.,.40,.10)*exp(-abs(q.x)*1.1)*exp(-dep*3.4)*.2*F*(1.-uK);col+=vec3(.006,.004,.003);}",
  // the wall, lit by the mouth and fading out before it reaches the text on the left
  "else{vec4 b=brick(w,vec2(118.,50.));vec3 alb=mix(vec3(.17,.085,.055),vec3(.31,.16,.095),b.y)*(.28+.72*b.x);alb*=.62+.5*vn(w*.075);alb*=mix(1.3,.72,b.w);",
  "float lit=exp(-dist*.82);float fade=smoothstep(-2.7,-1.05,q.x);lit*=fade;",
  "col=alb*lit*vec3(1.,.5,.22)*F*1.75;col+=vec3(.009,.006,.005);",
  // the ring of voussoirs round the arch, and the gold keystone diamond
  "if(d<.21){vec2 a=vec2(abs(q.x),q.y);float yc=Hd-APEX;float seg=q.y>yc?atan(a.y-yc,a.x+AA)*11.:q.y*6.+40.;float j=abs(fract(seg)-.5)*2.;float joint=1.-smoothstep(.84,.96,j);float tone=hash(vec2(floor(seg),step(0.,q.x)));vec3 st=mix(vec3(.34,.19,.12),vec3(.58,.33,.2),tone)*joint;float e=1.-smoothstep(.19,.21,d);col=mix(col,st*vec3(1.,.62,.32)*F*(exp(-d*5.5)*2.3+.22),e);}",
  "float dm=abs(q.x)*1.25+abs(q.y-(Hd+.105));if(dm<.062)col=mix(vec3(.98,.78,.31),vec3(.55,.3,.08),smoothstep(.03,.062,dm))*(.7+.5*F);",
  "}",
  "float band=1.-smoothstep(uL*.4,uL+210.,w.y);if(band>.001&&dist<9.){float hz=fbm(vec2(w.x*.0045+uT*.035,w.y*.013-uT*.018));col+=vec3(1.,.38,.12)*hz*hz*hz*band*exp(-dist*.5)*1.05*F*(1.-uK);}",
  "float ee=exp(-dist*.62);if(ee>.012&&w.y>uL*.8&&d>0.){float e1=embers(w,34.,1.25,1.);float e2=embers(w+17.,52.,.8,4.);col+=vec3(1.,.55,.18)*(e1+e2*.8)*ee*2.2*min(uH+.3,1.4)*smoothstep(.12,.34,uH);}",
  "col+=(hash(px+fract(uT))-.5)*.012;gl_FragColor=vec4(col,1.);}",
].join("\n");

export const UNIFORMS = ["uR", "uT", "uH", "uF", "uD", "uM", "uS", "uL", "uK", "uO"] as const;

/** Behind a sheet the fire draws only the window's top this tall (CSS px): the header (76) and the sheet's lit top edge
 *  (24, its rounded corners). Below it the sheet covers the wall, and the pilot light's glow is gone long before the
 *  sheet's side margins (the wall there is the shader's dark ambient, .k-stage's background). */
export const STAGE_BAND = 100;

/** The canvas for a window W x H (CSS px): its drawing buffer (cw x ch, at most 1000 px wide), its CSS height and the
 *  rows of the whole-window canvas below it (offset, uO). `band`: only the top STAGE_BAND px, the same pixels the whole
 *  canvas has there (same scale), so a sheet page repaints only the header band for each frame of the fire. */
export function stageCanvas(W: number, H: number, band: boolean) {
  const S = Math.min(0.72, 1000 / Math.max(W, 1));
  const cw = Math.max(2, Math.round(W * S)), fullCh = Math.max(2, Math.round(H * S));
  if (!band || H <= STAGE_BAND) return { cw, ch: fullCh, fullCh, cssH: H, offset: 0, S };
  const sy = fullCh / H;
  const ch = Math.min(fullCh, Math.ceil(STAGE_BAND * sy));
  return { cw, ch, fullCh, cssH: ch / sy, offset: fullCh - ch, S };
}

/** What the loop writes the flicker (--fire) onto: leaves only, never an element with the page under it (inbox 94: on
 *  .k-sheet every write restyled and repainted the whole page, about 11 times a second). The sheet's lit top edge
 *  (.k-sheet-rim, a strip with no children) and the Play title (.k-title, a word). */
export const FIRE_READERS = ".k-sheet-rim, .k-title";

/** On Play where WebKit composites nothing (flatDrawing): a frame at most this often (ms), about 15 a second instead of
 *  30. Each frame of the fire repaints the whole window there, the Play page and its list of mods with it, on the CPU
 *  (inbox 94 follow-up: in a 2560 x 1440 window the page had no time left between frames at 30 a second). */
export const FLAT_PLAY_MS = 66;

/** WebKit drawing without compositing: WebKitGTK with hardware acceleration off (webkit://gpu "Policy never":
 *  WEBKIT_DISABLE_DMABUF_RENDERER=1, which the Linux launcher sets, or WEBKIT_DISABLE_COMPOSITING_MODE=1). There every
 *  change is painted again on the CPU with everything over and under it, in one picture of the window (no layers).
 *  WebKit answers (-webkit-transform-3d) with false then; Chromium (WebView2) and WebKit with compositing say true. */
export function flatDrawing(): boolean {
  return typeof window !== "undefined" && typeof window.matchMedia === "function" && !window.matchMedia("(-webkit-transform-3d)").matches;
}

/** The running fire's wake-up (a still fire draws only when something changed); null while none runs. */
let wakeFire: (() => void) | null = null;

/** Tells the fire that stageInput changed: a still fire (reduced motion) draws its next frame then. */
export function wakeStage() {
  wakeFire?.();
}

/** The canvas behind everything. `root` is .k-app: its size, data-gl, the .k-door it moves (and --k on the door's label)
 *  and the flicker on FIRE_READERS. `enabled` false takes the no-WebGL path without asking for a context (give the
 *  canvas a new key to turn it back on). `onLit` hears whether the fire burns (the torch follows it).
 *  prefers-reduced-motion is followed while the fire runs (KDE's animation speed "Instant" turns it on): the fire stands
 *  still, writes no flicker and draws only when what it shows changed (wakeStage, a resize), with no timer meanwhile. */
export function Stage({ root, enabled = true, onLit }: { root: RefObject<HTMLDivElement>; enabled?: boolean; onLit?: (lit: boolean) => void }) {
  const ref = useRef<HTMLCanvasElement>(null);
  // the latest callback, without restarting the fire when the parent renders a new one
  const litCb = useRef(onLit);
  litCb.current = onLit;
  useEffect(() => {
    const app = root.current, canvas = ref.current;
    if (!app || !canvas) return;
    const motion = window.matchMedia?.("(prefers-reduced-motion: reduce)");
    const playMs = flatDrawing() ? FLAT_PLAY_MS : 33;
    let still = !!motion?.matches;
    let gl: WebGLRenderingContext | null = null;
    const U: Partial<Record<(typeof UNIFORMS)[number], WebGLUniformLocation | null>> = {};
    let raf = 0, nap = 0, parked = false;
    // the flicker's two readers (base.css --fire), looked up again when React replaced them
    let lit: HTMLElement[] = [];
    // back to base.css's .5 (a still fire, or none)
    const unflicker = () => {
      for (const e of app.querySelectorAll<HTMLElement>(FIRE_READERS)) e.style.removeProperty("--fire");
      lit = [];
    };
    const off = (why: string) => {
      gl = null;
      cancelAnimationFrame(raf);
      clearTimeout(nap);
      raf = nap = 0;
      delete app.dataset.gl;
      canvas.style.removeProperty("height");
      for (const p of ["transform", "width", "height"]) app.querySelector<HTMLElement>(".k-door")?.style.removeProperty(p);
      app.querySelector<HTMLElement>(".k-door-lab")?.style.removeProperty("--k");
      unflicker();
      litCb.current?.(false);
      if (why) console.warn("Hearth fire off:", why);
    };
    if (!enabled) { off(""); return; } // turned off, or not decided yet: the CSS mouth, quietly
    try { gl = canvas.getContext("webgl", { antialias: false, alpha: false, depth: false, stencil: false, powerPreference: "low-power" }); } catch { gl = null; }
    if (!gl) { off("no WebGL"); return; }
    const mk = (type: number, src: string) => {
      const o = gl!.createShader(type)!;
      gl!.shaderSource(o, src);
      gl!.compileShader(o);
      if (!gl!.getShaderParameter(o, gl!.COMPILE_STATUS)) { console.warn(gl!.getShaderInfoLog(o)); return null; }
      return o;
    };
    const vs = mk(gl.VERTEX_SHADER, VERT), fs = mk(gl.FRAGMENT_SHADER, FRAG);
    if (!vs || !fs) { off("shader"); return; }
    const prog = gl.createProgram()!;
    gl.attachShader(prog, vs); gl.attachShader(prog, fs); gl.linkProgram(prog);
    if (!gl.getProgramParameter(prog, gl.LINK_STATUS)) { off("link"); return; }
    gl.useProgram(prog);
    gl.bindBuffer(gl.ARRAY_BUFFER, gl.createBuffer());
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 3, -1, -1, 3]), gl.STATIC_DRAW);
    const loc = gl.getAttribLocation(prog, "p");
    gl.enableVertexAttribArray(loc);
    gl.vertexAttribPointer(loc, 2, gl.FLOAT, false, 0, 0);
    for (const n of UNIFORMS) U[n] = gl.getUniformLocation(prog, n);
    const lost = (e: Event) => { e.preventDefault(); off("context lost"); };
    canvas.addEventListener("webglcontextlost", lost);
    app.dataset.gl = "1";
    litCb.current?.(true);

    // the mouth's spring (k: 0 Play, 1 pilot light), the eased heat and the eased pointer
    let k = stageInput.play ? 0 : 1, kv = 0;
    let h = stageInput.play && !still ? 0.02 : target(stageInput);
    const m = [0, 0];
    let last = performance.now(), lastDraw = 0, lastCss = 0, lastDoor = "", lastSize = "", lastStill = "", lastHeight = "";
    let door: HTMLElement | null = null, lab: HTMLElement | null = null;
    const loop = (ms: number) => {
      if (!gl) return;
      raf = requestAnimationFrame(loop);
      if (document.hidden) { last = ms; return; }
      const dt = Math.min(0.05, Math.max(0.001, (ms - last) / 1000));
      last = ms;
      const i = stageInput;
      const tgt = i.play ? 0 : 1;
      const W = app.clientWidth, H = app.clientHeight;
      const heat = target(i);
      if (still) { k = tgt; kv = 0; h = heat; m[0] = i.mx; m[1] = i.my; }
      else {
        for (let s = 0; s < 2; s += 1) { kv += (-170 * (k - tgt) - 24 * kv) * (dt / 2); k += kv * (dt / 2); }
        h += (heat - h) * (1 - Math.exp(-dt * 2.2));
        m[0] += (i.mx - m[0]) * 0.05; m[1] += (i.my - m[1]) * 0.05;
      }
      const t = ms / 1000;
      // a still frame does not flicker: base.css's --fire stays at its .5
      const F = still ? 0.5 : Math.max(0, Math.min(1, 0.5 + 0.22 * Math.sin(t * 11.3) + 0.16 * Math.sin(t * 6.1 + 1.1) + 0.12 * Math.sin(t * 2.7 + 0.3)));
      // the Play mouth (40% of the width, 62.5% of the height, at most 800 px: base.css --kiln; 48 px from the right,
      // standing on the 84 px floor) and the pilot light
      const bw = Math.min(800, W * 0.4, H * 0.625), bh = bw * 0.88, bx = W - 48 - bw, by = H - 84 - bh, px = W - 60, py = 7;
      const dw = Math.max(14, bw + (36 - bw) * k), dh = dw * 0.88, dx = bx + (px - bx) * k, dy = by + (py - by) * k;
      const key = `${dx.toFixed(1)} ${dy.toFixed(1)} ${dw.toFixed(1)}`;
      if (!door?.isConnected || !lab?.isConnected) {
        door = app.querySelector<HTMLElement>(".k-door");
        lab = door?.querySelector<HTMLElement>(".k-door-lab") ?? null;
        lastDoor = "";
      }
      if (key !== lastDoor && door) {
        lastDoor = key;
        door.style.transform = `translate(${dx.toFixed(1)}px,${dy.toFixed(1)}px)`;
        door.style.width = `${dw.toFixed(1)}px`;
        door.style.height = `${dh.toFixed(1)}px`;
        // on its only reader (the label fades as the mouth shrinks), not on .k-app: there it restyled the whole window
        // every frame of the spring
        lab?.style.setProperty("--k", k.toFixed(3));
      }
      // behind a sheet with the mouth settled as the pilot light: only the header band (the canvas grows back to the whole
      // window on the frame Play is asked for, before the spring moves the mouth)
      const geo = stageCanvas(W, H, !i.play && Math.abs(k - 1) < 0.002 && Math.abs(kv) < 0.004);
      const size = `${W}x${H}x${geo.ch}`;
      if (still) {
        // drawn once for what it shows; then nothing (no frame, no timer) until wakeStage, a resize or motion again
        const what = `${key} ${heat} ${m[0].toFixed(3)} ${m[1].toFixed(3)} ${size}`;
        if (what === lastStill) { cancelAnimationFrame(raf); raf = 0; parked = true; return; }
        lastStill = what;
      } else {
        lastStill = "";
        const moving = Math.abs(kv) > 0.004 || Math.abs(k - tgt) > 0.002 || Math.abs(h - heat) > 0.02;
        const gap = moving || size !== lastSize ? 0 : i.game ? Infinity : i.play ? playMs : 110;
        if (gap === Infinity) {
          // the game runs and the frame stands: look again in 250 ms, not every frame
          cancelAnimationFrame(raf);
          nap = window.setTimeout(() => { nap = 0; raf = requestAnimationFrame(loop); }, 250);
          return;
        }
        if (ms - lastDraw < gap) return;
      }
      // the flicker, only on frames the fire draws: one repaint for both where WebKitGTK composites nothing
      if (!still && !i.game && ms - lastCss > 90) {
        lastCss = ms;
        if (!lit.length || lit.some((e) => !e.isConnected) || (i.play && !lit.some((e) => e.classList.contains("k-title")))) {
          lit = [...app.querySelectorAll<HTMLElement>(FIRE_READERS)];
        }
        for (const e of lit) e.style.setProperty("--fire", F.toFixed(3));
      }
      lastDraw = ms;
      lastSize = size;
      const { S, cw, ch } = geo;
      const cssH = geo.offset ? `${geo.cssH.toFixed(3)}px` : "";
      if (cssH !== lastHeight) { lastHeight = cssH; if (cssH) canvas.style.height = cssH; else canvas.style.removeProperty("height"); }
      if (canvas.width !== cw || canvas.height !== ch) { canvas.width = cw; canvas.height = ch; gl.viewport(0, 0, cw, ch); }
      gl.uniform2f(U.uR!, cw, geo.fullCh);
      gl.uniform1f(U.uO!, geo.offset);
      gl.uniform1f(U.uT!, still ? 12 : t % 1000);
      gl.uniform1f(U.uH!, h);
      gl.uniform1f(U.uF!, F);
      gl.uniform4f(U.uD!, (dx + dw / 2) * S, (H - (dy + dh)) * S, (dw / 2) * S, dh * S);
      gl.uniform2f(U.uM!, m[0], m[1]);
      gl.uniform1f(U.uS!, S);
      gl.uniform1f(U.uL!, 84);
      gl.uniform1f(U.uK!, k);
      gl.drawArrays(gl.TRIANGLES, 0, 3);
    };
    const resume = () => {
      if (!gl || !parked) return;
      parked = false;
      last = performance.now();
      raf = requestAnimationFrame(loop);
    };
    const onMotion = () => {
      still = !!motion?.matches;
      if (still) unflicker();
      resume();
    };
    motion?.addEventListener?.("change", onMotion);
    window.addEventListener("resize", resume);
    wakeFire = resume;
    raf = requestAnimationFrame(loop);
    return () => {
      gl = null;
      cancelAnimationFrame(raf);
      clearTimeout(nap);
      raf = nap = 0;
      canvas.removeEventListener("webglcontextlost", lost);
      motion?.removeEventListener?.("change", onMotion);
      window.removeEventListener("resize", resume);
      if (wakeFire === resume) wakeFire = null;
      delete app.dataset.gl;
      canvas.style.removeProperty("height");
      unflicker();
      litCb.current?.(false);
    };
  }, [root, enabled]);
  return <canvas ref={ref} className="k-stage-gl" aria-hidden />;
}
