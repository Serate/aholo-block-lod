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
#error{display:none;position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);color:#f66;font-size:14px;z-index:10;max-width:80%;background:rgba(0,0,0,.8);padding:20px;border-radius:8px;white-space:pre-wrap;text-align:center}
</style>
</head>
<body>
<div id="info">
  <h2>Block LOD Viewer</h2>
  <div class="stat">Blocks: <span id="statBlocks">-</span></div>
  <div class="stat">Total GS: <span id="statGs">-</span></div>
  <div class="stat">Points loaded: <span id="statLoaded">0</span></div>
  <div class="stat">Time: <span id="statTime">-</span></div>
</div>
<div id="controls">
  <button id="btnPoints" class="active">Show Points</button>
  <button id="btnFit">Fit View</button>
</div>
<div id="loading">Loading blocks...<div class="loading-bar"><div class="loading-bar-inner" id="loadBar"></div></div></div>
<div id="error"></div>
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

// ── RAD decoder ──

function readU32(d, o) { return d[o]|(d[o+1]<<8)|(d[o+2]<<16)|(d[o+3]<<24); }
function readU64(d, o) { return readU32(d,o)+readU32(d,o+4)*0x100000000; }

async function decompressRawDeflate(data) {
  const cs = new DecompressionStream('deflate-raw');
  const w = cs.writable.getWriter();
  w.write(data); w.close();
  const r = cs.readable.getReader();
  const ch = [];
  for(;;){const{done,value}=await r.read();if(done)break;ch.push(value)}
  const t = ch.reduce((s,c)=>s+c.length,0);
  const o = new Uint8Array(t); let p=0; for(const c of ch){o.set(c,p);p+=c.length}
  return o;
}

async function decodeProperty(data) {
  if(data.length<1) return new Uint8Array(0);
  if(data[0]===0) return data.slice(1);
  if(data[0]===1) return decompressRawDeflate(data.slice(1));
  return data;
}

function f16(h) {
  const s=(h&0x8000)<<16; let e=(h>>10)&0x1f,m=h&0x3ff;
  if(e===0){e=1;while(!(m&0x400)&&m){m<<=1;e--}m&=0x3ff;e+=112}
  else if(e===31)e=255;else e+=112;
  const b=s|(e<<23)|(m<<13);const a=new Float32Array(1);a[0]=b;return a[0];
}

/**
 * Decode .rad file → positions Float32Array + colors Float32Array.
 * Returns { positions, colors, count } or null on error.
 */
async function decodeRadToArrays(data) {
  try {
    if(data.length<8||readU32(data,0)!==0x30444152) throw new Error('Bad magic');
    const ml=readU32(data,4), me=8+((ml+7)&~7);
    if(data.length<me) throw new Error('Truncated header');
    const meta=JSON.parse(new TextDecoder().decode(data.slice(8,8+ml)));
    const total=meta.count, chunkSize=meta.chunkSize||16384, chunks=meta.chunks;

    // Pre-allocate
    const pos = new Float32Array(total * 3);
    const col = new Float32Array(total * 3);
    let gsCount = 0;
    const chunksStart = me;

    for(const cr of chunks){
      if(gsCount>=total) break;
      const co=chunksStart+cr.offset;
      let cd=data.slice(co,co+Math.min(cr.bytes,data.length-co));
      if(cd.length<8||readU32(cd,0)!==0x43444152) break;
      const cml=readU32(cd,4), cme=8+((cml+7)&~7);
      const pb=readU64(cd,cme), ps=cme+8;
      const cj=JSON.parse(new TextDecoder().decode(cd.slice(8,8+cml)));
      let centerF32=null, alphaU8=null, rgbU8=null;

      for(const prop of cj.properties){
        if(prop.offset+prop.bytes>pb) continue;
        const pd=cd.slice(ps+prop.offset,ps+prop.offset+prop.bytes);
        const dc=await decodeProperty(pd);

        if(prop.property==='center'&&dc.length>=12){
          const f32=new Float32Array(dc.buffer,dc.byteOffset,dc.length/4);
          const cnt=Math.min(Math.floor(dc.length/12),total-gsCount);
          centerF32={data:f32,count:cnt};
        }else if(prop.property==='alpha'&&dc.length>0){
          alphaU8=dc;
        }else if(prop.property==='rgb'&&dc.length>=3){
          rgbU8=dc;
        }
      }

      if(!centerF32) break;
      const n=centerF32.count;
      const d=centerF32.data;
      for(let i=0;i<n&&gsCount+i<total;i++){
        pos[(gsCount+i)*3]=d[i];
        pos[(gsCount+i)*3+1]=d[n+i];
        pos[(gsCount+i)*3+2]=d[n*2+i];
        if(rgbU8){
          col[(gsCount+i)*3]=rgbU8[i*3]/255;
          col[(gsCount+i)*3+1]=rgbU8[i*3+1]/255;
          col[(gsCount+i)*3+2]=rgbU8[i*3+2]/255;
        }else{
          col[(gsCount+i)*3]=0.7;col[(gsCount+i)*3+1]=0.7;col[(gsCount+i)*3+2]=0.7;
        }
        // Apply alpha
        if(alphaU8&&alphaU8[i]>0){
          const a=alphaU8[i]/255;
          for(let c=0;c<3;c++)col[(gsCount+i)*3+c]*=a;
        }
      }
      gsCount+=n;
    }
    return {positions:pos.slice(0,gsCount*3),colors:col.slice(0,gsCount*3),count:gsCount};
  } catch(e) { console.error('decodeRad error:', e); throw e; }
}

// ── Viewer ──

const canvas = document.getElementById('canvas');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.setSize(window.innerWidth, window.innerHeight);
renderer.setClearColor(0x1a1a2e);
renderer.outputColorSpace = THREE.SRGBColorSpace;

const scene = new THREE.Scene();
const camera = new THREE.PerspectiveCamera(60, window.innerWidth/window.innerHeight, 0.01, 100000);
const controls = new OrbitControls(camera, canvas);
controls.enableDamping = true;
controls.dampingFactor = 0.1;
controls.target.set(0,0,0);

let pointsMesh = null;

function showError(msg) {
  const el = document.getElementById('error');
  el.textContent = msg; el.style.display = 'block';
}

async function loadBlocks() {
  try {
    const t0 = performance.now();
    const metaResp = await fetch('lod-meta.json');
    if(!metaResp.ok) throw new Error('lod-meta.json not found');
    const meta = await metaResp.json();
    document.getElementById('statBlocks').textContent = meta.tree.length;
    document.getElementById('statGs').textContent = meta.counts.toLocaleString();

    // Accumulate all positions & colors
    let allPos = null, allCol = null, totalPts = 0;

    for(let bi=0;bi<meta.tree.length;bi++){
      const fn = meta.files[bi];
      const resp = await fetch(fn);
      if(!resp.ok) throw new Error('Failed to fetch '+fn);
      const buf = await resp.arrayBuffer();
      const result = await decodeRadToArrays(new Uint8Array(buf));
      if(!result||result.count===0){
        console.warn('Block '+bi+': empty');
        continue;
      }
      // Merge
      if(allPos===null){
        allPos = result.positions;
        allCol = result.colors;
      } else {
        const np = new Float32Array(allPos.length + result.positions.length);
        np.set(allPos); np.set(result.positions, allPos.length);
        allPos = np;
        const nc = new Float32Array(allCol.length + result.colors.length);
        nc.set(allCol); nc.set(result.colors, allCol.length);
        allCol = nc;
      }
      totalPts += result.count;
      document.getElementById('statLoaded').textContent = totalPts.toLocaleString();
      document.getElementById('loadBar').style.width = ((bi+1)/meta.tree.length*100)+'%';
    }

    document.getElementById('loading').style.display = 'none';

    if(!allPos||totalPts===0){
      showError('No points loaded');
      return;
    }

    console.log('Total points:', totalPts);
    console.log('First 5 positions:', allPos[0], allPos[1], allPos[2], allPos[3], allPos[4], allPos[5], allPos[6], allPos[7], allPos[8], allPos[9], allPos[10], allPos[11], allPos[12], allPos[13], allPos[14]);
    console.log('First 5 colors:', allCol[0], allCol[1], allCol[2], allCol[3], allCol[4], allCol[5], allCol[6], allCol[7], allCol[8], allCol[9], allCol[10], allCol[11], allCol[12], allCol[13], allCol[14]);
    document.getElementById('statLoaded').textContent = totalPts.toLocaleString();
    document.getElementById('statTime').textContent = ((performance.now()-t0)/1000).toFixed(1)+'s';

    // Create point cloud
    const geom = new THREE.BufferGeometry();
    geom.setAttribute('position', new THREE.Float32BufferAttribute(allPos, 3));
    geom.setAttribute('color', new THREE.Float32BufferAttribute(allCol, 3));

    // Compute bounds for camera
    const box = new THREE.Box3().setFromArray(allPos);
    const center = new THREE.Vector3();
    box.getCenter(center);
    const size = box.getSize(new THREE.Vector3());
    const maxDim = Math.max(size.x, size.y, size.z);
    console.log('Scene size:', size.x.toFixed(1), size.y.toFixed(1), size.z.toFixed(1));

    const mat = new THREE.PointsMaterial({
      size: maxDim * 0.002, // adaptive point size
      vertexColors: true,
      sizeAttenuation: true,
      transparent: true,
      opacity: 0.95,
    });

    pointsMesh = new THREE.Points(geom, mat);
    scene.add(pointsMesh);

    // Fit camera
    const dist = maxDim * 1.8;
    camera.position.set(dist*0.6, dist*0.4, dist*0.8);
    controls.target.copy(center);
    controls.update();
    console.log('Camera pos:', camera.position);

  } catch(e) {
    console.error('loadBlocks error:', e);
    showError('Error: '+e.message);
    document.getElementById('loading').style.display = 'none';
  }
}

// Controls
document.getElementById('btnPoints').addEventListener('click', () => {
  if(!pointsMesh) return;
  pointsMesh.visible = !pointsMesh.visible;
  document.getElementById('btnPoints').classList.toggle('active', pointsMesh.visible);
});

document.getElementById('btnFit').addEventListener('click', () => {
  if(!pointsMesh) return;
  const box = new THREE.Box3().setFromObject(pointsMesh);
  const c = new THREE.Vector3(); box.getCenter(c);
  const s = new THREE.Vector3(); box.getSize(s);
  const d = Math.max(s.x,s.y,s.z)*1.8;
  controls.target.copy(c);
  camera.position.set(d*0.6,d*0.4,d*0.8);
  controls.update();
});

window.addEventListener('resize', () => {
  camera.aspect = window.innerWidth/window.innerHeight;
  camera.updateProjectionMatrix();
  renderer.setSize(window.innerWidth, window.innerHeight);
});

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
