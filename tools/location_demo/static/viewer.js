'use strict';

const COLORS = { ble: '#0077c8', wifi: '#2ca02c', gnss: '#ff7f0e', other: '#888888' };
const POLL_MS = 3000;

const map = L.map('map').setView([20, 0], 2);
L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png', {
  maxZoom: 19,
  attribution: '&copy; OpenStreetMap contributors',
}).addTo(map);

const layer = L.layerGroup().addTo(map);
let fitted = false;

function methodClass(method) {
  const m = String(method || '').toLowerCase();
  if (m.includes('wifi') || m.includes('wi-fi')) return 'wifi';
  if (m.includes('gnss') || m.includes('gps')) return 'gnss';
  if (m.includes('ble') || m.includes('bluetooth') || m.includes('gateway')) return 'ble';
  return 'other';
}

function parseTime(value) {
  if (value == null || value === '') return null;
  if (typeof value === 'number') return value < 1e12 ? value * 1000 : value;
  let text = String(value);
  // A time without a zone is UTC, as in the AWS messages
  if (!/(Z|[+-]\d\d:?\d\d)$/i.test(text)) text += 'Z';
  const ms = Date.parse(text);
  return Number.isNaN(ms) ? null : ms;
}

function ago(ms) {
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 5) return 'just now';
  if (s < 60) return `${s} s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m} min ${s % 60} s ago`;
  const h = Math.floor(m / 60);
  if (h < 24) return `${h} h ${m % 60} min ago`;
  return `${Math.floor(h / 24)} d ago`;
}

function timeElement(position) {
  const ms = parseTime(position.timestamp) ?? parseTime(position.received);
  const element = document.createElement('span');
  if (ms == null) {
    element.textContent = 'unknown time';
    return element;
  }
  element.className = 'ago';
  element.dataset.ms = String(ms);
  element.title = new Date(ms).toLocaleString();
  element.textContent = ago(ms);
  return element;
}

function describe(position) {
  const element = document.createElement('span');
  const parts = [position.method || 'unknown method'];
  if (position.accuracy_m != null) parts.push(`±${Math.round(position.accuracy_m)} m`);
  element.append(document.createTextNode(`${parts.join(' · ')} · `), timeElement(position));
  return element;
}

function textElement(tag, className, text) {
  const element = document.createElement(tag);
  element.className = className;
  element.textContent = text;
  return element;
}

setInterval(() => {
  for (const element of document.querySelectorAll('.ago')) {
    element.textContent = ago(Number(element.dataset.ms));
  }
}, 1000);

function render(data) {
  layer.clearLayers();
  const list = document.getElementById('devices');
  list.replaceChildren();
  const bounds = [];

  for (const device of data.devices) {
    const positions = device.positions;
    const last = positions[positions.length - 1];

    positions.forEach((position, index) => {
      const latest = index === positions.length - 1;
      const color = COLORS[methodClass(position.method)];
      const point = [position.lat, position.lon];
      if (latest && position.accuracy_m) {
        L.circle(point, { radius: position.accuracy_m, color, weight: 1, fillOpacity: 0.15 })
          .addTo(layer);
      }
      const marker = L.circleMarker(point, {
        radius: latest ? 8 : 4,
        color,
        fillColor: color,
        fillOpacity: latest ? 0.9 : 0.4,
        weight: latest ? 2 : 1,
      }).addTo(layer);
      const popup = document.createElement('div');
      popup.append(textElement('strong', '', device.name), document.createElement('br'),
                   describe(position));
      marker.bindPopup(popup);
      if (latest) {
        marker.bindTooltip(device.name, { permanent: true, direction: 'right', offset: [10, 0] });
      }
      bounds.push(point);
    });

    const item = document.createElement('div');
    item.className = 'device';
    const detail = textElement('div', 'detail', last ? '' : 'no position yet');
    if (last) detail.append(describe(last));
    item.append(textElement('div', 'name', device.name), detail);
    if (last) {
      item.addEventListener('click', () => map.setView([last.lat, last.lon], 16));
    }
    list.append(item);
  }

  if (!fitted && bounds.length) {
    map.fitBounds(bounds, { maxZoom: 16, padding: [40, 40] });
    fitted = true;
  }
}

async function poll() {
  const status = document.getElementById('status');
  try {
    const response = await fetch('/api/positions', { cache: 'no-store' });
    render(await response.json());
    status.textContent = `Updated ${new Date().toLocaleTimeString()}`;
  } catch (err) {
    status.textContent = `Viewer not reachable: ${err}`;
  }
}

poll();
setInterval(poll, POLL_MS);
