#!/usr/bin/env node
/**
 * Dev server for block viewer.
 * Serves block data and a Three.js-based viewer HTML page.
 *
 * Usage:
 *   node scripts/serve-block-viewer.mjs <block-data-dir> [port]
 *
 * Then open http://localhost:3000 in browser.
 */
import { createServer } from 'node:http';
import { readFileSync, existsSync, readdirSync } from 'node:fs';
import { join, extname, resolve } from 'node:path';

const dataDir = resolve(process.argv[2] || '.');
const port = parseInt(process.argv[3] || '3000', 10);

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'application/javascript; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.rad': 'application/octet-stream',
    '.wasm': 'application/wasm',
    '.png': 'image/png',
    '.jpg': 'image/jpeg',
    '.svg': 'image/svg+xml',
    '.css': 'text/css; charset=utf-8',
};

const server = createServer((req, res) => {
    const url = new URL(req.url, `http://localhost:${port}`);
    let path = url.pathname;

    // Serve viewer page at root
    if (path === '/' || path === '/index.html') {
        res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
        res.end(getViewerHTML());
        return;
    }

    // Serve files from data directory
    const filePath = join(dataDir, path);
    if (existsSync(filePath)) {
        const ext = extname(filePath);
        const mime = MIME[ext] || 'application/octet-stream';
        // For .rad files, add CORS headers
        const headers = {
            'Content-Type': mime,
            'Access-Control-Allow-Origin': '*',
            'Cache-Control': 'no-cache',
        };
        res.writeHead(200, headers);
        res.end(readFileSync(filePath));
        return;
    }

    res.writeHead(404);
    res.end('Not Found');
});

server.listen(port, () => {
    console.log(`Block viewer server running at http://localhost:${port}`);
    console.log(`Serving data from: ${dataDir}`);
    // List available .rad files
    try {
        const files = readdirSync(dataDir).filter(f => f.endsWith('.rad'));
        console.log(`  ${files.length} .rad files found`);
        if (existsSync(join(dataDir, 'lod-meta.json'))) {
            console.log('  lod-meta.json found');
        }
    } catch { /* ignore */ }
});

function getViewerHTML() {
    return `<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Block LOD Viewer</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{background:#111;color:#eee;font-family:system-ui,sans-serif;overflow:hidden}
#info{position:absolute;top:12px;left:12px;background:rgba(0,0,0,.75);padding:12px 16px;border-radius:8px;font-size:13px;line-height:1.6;pointer-events:none;z-index:10;max-width:360px}
#info h2{font-size:15px;margin-bottom:4px;color:#8cf}
#info .stat{opacity:.8}
#info .stat span{color:#8cf}
#controls{position:absolute;bottom:20px;left:50%;transform:translateX(-50%);display:flex;gap:10px;z-index:10}
#controls button{background:rgba(255,255,255,.12);color:#fff;border:1px solid rgba(255,255,255,.2);padding:8px 16px;border-radius:6px;cursor:pointer;font-size:13px}
#controls button:hover{background:rgba(255,255,255,.2)}
#controls button.active{background:rgba(100,180,255,.3);border-color:#8cf}
#canvas{display:block;width:100vw;height:100vh}
#loading{position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);color:#888;font-size:18px;z-index:5}
.loading-bar{width:200px;height:3px;background:#333;border-radius:2px;margin:12px auto 0;overflow:hidden}
.loading-bar-inner{height:100%;width:0%;background:#8cf;transition:width .3s}
</style>
</head>
<body>
<div id="info">
  <h2>🔲 Block LOD Viewer</h2>
  <div class="stat">Blocks: <span id="statBlocks">-</span></div>
  <div class="stat">Total GS: <span id="statGs">-</span></div>
  <div class="stat">Rendered: <span id="statRendered">-</span></div>
</div>
<div id="controls">
  <button id="btnPoints" class="active">Points</button>
  <button id="BtnResetView">Reset View</button>
</div>
<div id="loading">Loading blocks...<div class="loading-bar"><div class="loading-bar-inner" id="loadBar"></div></div></div>
<canvas id="canvas"></canvas>

<script type="importmap">
{
  "imports": {
    "three": "https://cdn.jsdelivr.net/npm/three@0.170.0/build/three.module.js",
    "three/addons/": "https://cdn.jsdelivr.net/npm/three@0.170.0/examples/jsm/"
  }
}
</script>
<script type="module">
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { GUI } from 'three/addons/libs/lil-gui.module.min.js';

// ── RAD decoder (browser-compatible) ──

function readU32LE(d, o) { return d[o]|(d[o+1]<<8)|(d[o+2]<<16)|(d[o+3]<<24); }
function readU64LE(d, o) { return readU32LE(d,o)+readU32LE(d,o+4)*0x100000000; }

async function decompressRawDeflate(data) {
  const cs = new DecompressionStream('deflate-raw');
  const w = cs.writable.getWriter();
  w.write(data); w.close();
  const r = cs.readable.getReader();
  const chunks = [];
  while (true) { const {done,value}=await r.read(); if(done)break; chunks.push(value); }
  const total = chunks.reduce((s,c)=>s+c.length,0);
  const result = new Uint8Array(total);
  let off=0; for(const c of chunks){result.set(c,off);off+=c.length;}
  return result;
}

async function decodeProperty(data) {
  if(data.length<1) return new Uint8Array(0);
  const tag=data[0];
  if(tag===0) return data.slice(1);
  if(tag===1) return decompressRawDeflate(data.slice(1));
  return data;
}

async function decodeRad(data) {
  if(data.length<8||readU32LE(data,0)!==0x30444152) return null;
  const metaLen=readU32LE(data,4);
  const metaEnd=8+((metaLen+7)&~7);
  if(data.length<metaEnd) return null;
  const meta=JSON.parse(new TextDecoder().decode(data.slice(8,8+metaLen)));
  const total=meta.count, chunkSize=meta.chunkSize||16384, chunks=meta.chunks;
  const result={center:[],rgba:[],scale:[]};
  const chunksStart=metaEnd;
  let base=0;
  for(const cr of chunks){
    if(base>=total) break;
    const chunkOff=chunksStart+cr.offset;
    const chunkData=data.slice(chunkOff,chunkOff+Math.min(cr.bytes,data.length-chunkOff));
    if(chunkData.length<8) break;
    const cm=readU32LE(chunkData,0);
    if(cm!==0x43444152) break;
    const cmLen=readU32LE(chunkData,4);
    const cmEnd=8+((cmLen+7)&~7);
    const payloadBytes=readU64LE(chunkData,cmEnd);
    const payloadStart=cmEnd+8;
    const cmJson=JSON.parse(new TextDecoder().decode(chunkData.slice(8,8+cmLen)));
    for(const prop of cmJson.properties){
      if(prop.offset+prop.bytes>payloadBytes) continue;
      const pd=chunkData.slice(payloadStart+prop.offset,payloadStart+prop.offset+prop.bytes);
      const decomp=await decodeProperty(pd);
      const nItems=Math.min(decomp.length/4,total-base);
      if(prop.property==='center'&&decomp.length>=12){
        const f32=new Float32Array(decomp.buffer,decomp.byteOffset,decomp.length/4);
        const pdim=Math.min(decomp.length/12,total-base);
        for(let i=0;i<pdim;i++){
          result.center[base+i*3]=f32[i];   // x
          result.center[base+i*3+1]=f32[pdim+i]; // y
          result.center[base+i*3+2]=f32[pdim*2+i]; // z
        }
      }else if(prop.property==='rgb'&&decomp.length>=3){
        const n=Math.min(Math.floor(decomp.length/3),total-base);
        for(let i=0;i<n;i++){
          result.rgba[base+i*4]=decomp[i*3];
          result.rgba[base+i*4+1]=decomp[i*3+1];
          result.rgba[base+i*4+2]=decomp[i*3+2];
        }
      }else if(prop.property==='alpha'){
        const n=Math.min(decomp.length,total-base);
        for(let i=0;i<n;i++) result.rgba[base+i*4+3]=decomp[i];
      }else if(prop.property==='scale'&&decomp.length>=3){
        const n=Math.min(Math.floor(decomp.length/3),total-base);
        for(let i=0;i<n;i++){
          result.scale[base+i*3]=decomp[i*3];
          result.scale[base+i*3+1]=decomp[i*3+1];
          result.scale[base+i*3+2]=decomp[i*3+2];
        }
      }
    }
    base+=chunkSize;
    if(base>total) base=total;
  }
  return result;
}

// ── Viewer ──

const canvas = document.getElementById('canvas');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.setClearColor(0x111111);

const scene = new THREE.Scene();

const camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 10000);
camera.position.set(10, 10, 15);

const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
controls.dampingFactor = 0.1;
controls.target.set(0, 0, 0);

const ambientLight = new THREE.AmbientLight(0x404040);
scene.add(ambientLight);
const dirLight = new THREE.DirectionalLight(0xffffff, 1);
dirLight.position.set(1, 2, 1);
scene.add(dirLight);

// Grid helper
scene.add(new THREE.GridHelper(20, 20, 0x444444, 0x222222));

let allBlocks = [];
let showPoints = true;
let pointsMesh = null;
let blockMeshes = [];

const loadingEl = document.getElementById('loading');
const loadBar = document.getElementById('loadBar');

function f16toF32(h) {
  const s=(h&0x8000)<<16;
  let e=(h>>10)&0x1f,m=h&0x3ff;
  if(e===0){e=1;while(!(m&0x400)&&m){m<<=1;e--;}m&=0x3ff;e+=112;}
  else if(e===31)e=255;else e+=112;
  const bits=s|(e<<23)|(m<<13);
  const arr=new Float32Array(1);arr[0]=bits;return arr[0];
}

function ln0r8ToF32(u){return Math.exp((u/255)*21-12);}

async function loadBlocks() {
  try {
    const metaResp = await fetch('lod-meta.json');
    const meta = await metaResp.json();
    document.getElementById('statBlocks').textContent = meta.tree.length;
    document.getElementById('statGs').textContent = meta.counts.toLocaleString();
    loadingEl.style.display = 'block';

    const allPos = [];
    const allColors = [];
    let totalLoaded = 0;

    for (let bi = 0; bi < meta.tree.length; bi++) {
      const fileName = meta.files[bi];
      const resp = await fetch(fileName);
      const buf = await resp.arrayBuffer();
      const decoded = await decodeRad(new Uint8Array(buf));

      // Extract positions and colors
      const centerF32 = new Float32Array(decoded.center.length);
      for (let i = 0; i < decoded.center.length / 3; i++) {
        centerF32[i*3]   = f16toF32(decoded.center[i*3]);
        centerF32[i*3+1] = f16toF32(decoded.center[i*3+1]);
        centerF32[i*3+2] = f16toF32(decoded.center[i*3+2]);
      }
      for (let i = 0; i < decoded.center.length / 3; i++) {
        allPos.push(centerF32[i*3], centerF32[i*3+1], centerF32[i*3+2]);
        allColors.push(
          (decoded.rgba?.[i*4]??128) / 255,
          (decoded.rgba?.[i*4+1]??128) / 255,
          (decoded.rgba?.[i*4+2]??128) / 255,
        );
      }

      totalLoaded += decoded.center.length / 3;
      loadBar.style.width = Math.min(100, ((bi + 1) / meta.tree.length) * 100) + '%';
      document.getElementById('statRendered').textContent = totalLoaded.toLocaleString();
    }

    // Create point cloud
    const geom = new THREE.BufferGeometry();
    geom.setAttribute('position', new THREE.Float32BufferAttribute(allPos, 3));
    geom.setAttribute('color', new THREE.Float32BufferAttribute(allColors, 3));

    const mat = new THREE.PointsMaterial({
      size: 0.05,
      vertexColors: true,
      sizeAttenuation: true,
      blending: THREE.AdditiveBlending,
      depthWrite: false,
      transparent: true,
      opacity: 0.9,
    });
    pointsMesh = new THREE.Points(geom, mat);
    scene.add(pointsMesh);
    allBlocks.push(pointsMesh);

    // Compute scene center
    const box = new THREE.Box3().setFromObject(pointsMesh);
    const center = new THREE.Vector3();
    box.getCenter(center);
    controls.target.copy(center);

    // Auto-fit camera
    const size = box.getSize(new THREE.Vector3());
    const maxDim = Math.max(size.x, size.y, size.z);
    const dist = maxDim * 1.5;
    camera.position.set(dist * 0.6, dist * 0.4, dist * 0.8);
    camera.lookAt(center);
    controls.update();

    loadingEl.style.display = 'none';
  } catch(e) {
    loadingEl.textContent = 'Error: ' + e.message;
    console.error(e);
  }
}

// Controls
document.getElementById('btnPoints').addEventListener('click', () => {
  showPoints = !showPoints;
  if (pointsMesh) pointsMesh.visible = showPoints;
  document.getElementById('btnPoints').classList.toggle('active', showPoints);
});

document.getElementById('BtnResetView').addEventListener('click', () => {
  if (allBlocks.length > 0) {
    const box = new THREE.Box3().setFromObject(allBlocks[0]);
    const center = new THREE.Vector3();
    box.getCenter(center);
    controls.target.copy(center);
    const size = box.getSize(new THREE.Vector3());
    const dist = Math.max(size.x, size.y, size.z) * 1.5;
    camera.position.set(dist * 0.6, dist * 0.4, dist * 0.8);
    controls.update();
  }
});

// Resize
window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth / window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});

// Render loop
function animate() {
  controls.update();
  renderer.render(scene, camera);
  requestAnimationFrame(animate);
}

loadBlocks();
animate();
</script>
</body>
</html>`;
}
