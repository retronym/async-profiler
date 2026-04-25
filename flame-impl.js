// Copyright The async-profiler authors
// SPDX-License-Identifier: Apache-2.0
'use strict';
let root, px, pattern;
let level0 = 0, left0 = 0, width0 = 0, d = 0;
let nav = [], navIndex, matchval;
let inverted = false;
const U = undefined;

const canvas = document.getElementById('canvas');
const c = canvas.getContext('2d');
const hl = document.getElementById('hl');
const status = document.getElementById('status');

// Visual zoom/pan state
let zoomFactor = 1;
let panSamples = 0;
let x0Rendered = 0;

// Track whether stacks have been removed (to show restore button)
let stacksRemoved = false;

let canvasWidth = canvas.offsetWidth;
let canvasHeight = canvas.offsetHeight;

function setupCanvas() {
	const dpr = devicePixelRatio || 1;
	canvas.width = canvasWidth * dpr;
	canvas.height = canvasHeight * dpr;
	c.scale(dpr, dpr);
	c.font = document.body.style.font;
}
setupCanvas();

// Re-render on container resize, debounced to avoid thrashing
let resizeTimer;
new ResizeObserver(() => {
	clearTimeout(resizeTimer);
	resizeTimer = setTimeout(() => {
		canvasWidth = canvas.offsetWidth;
		canvasHeight = canvas.offsetHeight;
		setupCanvas();
		if (root) render(root);
	}, 100);
}).observe(canvas);

const palette = [
	[0xb2e1b2, 20, 20, 20],
	[0x50e150, 30, 30, 30],
	[0x50cccc, 30, 30, 30],
	[0xe15a5a, 30, 40, 40],
	[0xc8c83c, 30, 30, 10],
	[0xe17d00, 30, 30,  0],
	[0xcce880, 20, 20, 20],
];

function getColor(p) {
	const v = Math.random();
	return '#' + (p[0] + ((p[1] * v) << 16 | (p[2] * v) << 8 | (p[3] * v))).toString(16);
}

function getDiffColor(diff) {
	if (diff === U) return '#ffdd33';
	if (diff === 0) return '#e0e0e0';
	const v = Math.round(128 * (maxdiff - Math.abs(diff)) / maxdiff) + 96;
	return diff > 0 ? 'rgb(255,' + v + ',' + v + ')' : 'rgb(' + v + ',' + v + ',255)';
}

function f(key, level, left, width, inln, c1, int) {
	levels[level0 = level].push({level, left: left0 += left, width: width0 = width || width0,
		color: maxdiff >= 0 ? getDiffColor(d) : getColor(palette[key & 7]),
		title: cpool[key >>> 3],
		details: (d ? (d > 0 ? ', +' : ', ') + d : '') + (int ? ', int=' + int : '') + (c1 ? ', c1=' + c1 : '') + (inln ? ', inln=' + inln : '')
	});
}

function u(key, width, inln, c1, int) {
	f(key, level0 + 1, 0, width, inln, c1, int)
}

function n(key, width, inln, c1, int) {
	f(key, level0, width0, width, inln, c1, int)
}

function samples(n) {
	return n === 1 ? '1 sample' : n.toString().replace(/\B(?=(\d{3})+(?!\d))/g, ',') + ' samples';
}

function pct(a, b) {
	return a >= b ? '100' : (100 * a / b).toFixed(2);
}

function findFrame(frames, x) {
	let left = 0;
	let right = frames.length - 1;

	while (left <= right) {
		const mid = (left + right) >>> 1;
		const f = frames[mid];

		if (f.left > x) {
			right = mid - 1;
		} else if (f.left + f.width <= x) {
			left = mid + 1;
		} else {
			return f;
		}
	}

	if (frames[left] && (frames[left].left - x) * px < 0.5) return frames[left];
	if (frames[right] && (x - (frames[right].left + frames[right].width)) * px < 0.5) return frames[right];

	return null;
}

function removeStack(left, width) {
	for (let h = 0; h < levels.length; h++) {
		const frames = levels[h], newFrames = [];
		for (let i = 0; i < frames.length; i++) {
			const fr = frames[i];
			if (fr.left >= left + width) {
				fr.left -= width;
			} else if (fr.left + fr.width > left) {
				if ((fr.width -= width) <= 0 && h) continue;
			}
			newFrames.push(fr);
		}
		levels[h] = newFrames;
	}
	stacksRemoved = true;
	document.getElementById('restoreBtn').disabled = false;
}

function updateZoomDisplay() {
	document.getElementById('zoomlevel').textContent = Math.round(zoomFactor * 100) + '%';
	document.getElementById('zoomreset').disabled = (zoomFactor === 1);
}

function clampPan() {
	if (root && zoomFactor > 1) {
		panSamples = Math.max(0, Math.min(panSamples, root.width * (1 - 1 / zoomFactor)));
	}
}

function zoomAt(factor, canvasX) {
	const sampleAtX = canvasX / px + x0Rendered;
	zoomFactor = Math.max(1, zoomFactor * factor);
	if (zoomFactor === 1) {
		panSamples = 0;
	} else {
		const newPx = canvasWidth * zoomFactor / root.width;
		panSamples = sampleAtX - root.left - canvasX / newPx;
		clampPan();
	}
	render(root);
}

function keepOnlyMatched(term) {
	if (!nav || nav.length === 0) return;
	if (term) { filterHistory.push(term); updateFilterChips(); }
	let prefix = 0;
	const kept = nav.map(fr => {
		const entry = {left: fr.left, right: fr.left + fr.width, prefix};
		prefix += fr.width;
		return entry;
	});

	for (let h = 0; h < levels.length; h++) {
		const newFrames = [];
		for (const fr of levels[h]) {
			const frL = fr.left, frR = fr.left + fr.width;
			let newWidth = 0, firstLeft = -1;
			for (const r of kept) {
				if (r.right <= frL) continue;
				if (r.left >= frR) break;
				const intL = Math.max(r.left, frL);
				const intR = Math.min(r.right, frR);
				if (firstLeft < 0) firstLeft = r.prefix + (intL - r.left);
				newWidth += intR - intL;
			}
			if (newWidth > 0) newFrames.push({...fr, left: firstLeft, width: newWidth});
		}
		levels[h] = newFrames;
	}
	stacksRemoved = true;
	document.getElementById('restoreBtn').disabled = false;
	zoomFactor = 1; panSamples = 0;
	// Re-run search on the reduced data so nav and matchpct stay accurate
	// const term = document.getElementById('searchinput').value;
	// if (term) {
	// 	search(term);
	// } else {
	// }
	render(levels[0][0]);
}

function restoreStacks() {
	for (let h = 0; h < levels.length; h++) {
		levels[h] = originalLevels[h].map(fr => ({...fr}));
	}
	stacksRemoved = false;
	filterHistory = [];
	updateFilterChips();
	document.getElementById('restoreBtn').disabled = true;
	zoomFactor = 1; panSamples = 0;
	render(levels[0][0]);
}

function render(newRoot, nav) {
	const bg = getComputedStyle(document.documentElement).getPropertyValue('--bg');

	if (root) {
		c.fillStyle = bg;
		c.fillRect(0, 0, canvasWidth, canvasHeight);
	}

	const prevRoot = root;
	root = newRoot || levels[0][0];
	if (root !== prevRoot) {
		zoomFactor = 1;
		panSamples = 0;
	} else {
		clampPan();
	}

	px = canvasWidth * zoomFactor / root.width;
	x0Rendered = root.left + panSamples;

	const x0 = x0Rendered;
	const x1 = x0 + root.width / zoomFactor;
	const marked = [];

	function mark(f) {
		return marked[f.left] || (marked[f.left] = f);
	}

	function totalMarked() {
		let total = 0;
		let left = 0;
		Object.keys(marked).sort(function(a, b) { return a - b; }).forEach(function(x) {
			if (+x >= left) {
				const m = marked[x];
				if (nav) nav.push(m);
				total += m.width;
				left = +x + m.width;
			}
		});
		return total;
	}

	function drawFrame(f, y) {
		if (f.left < x1 && f.left + f.width > x0) {
			c.fillStyle = pattern && f.title.match(pattern) && mark(f) ? '#ee00ee' : f.color;
			c.fillRect((f.left - x0) * px, y, f.width * px, 15);

			if (f.width * px >= 21) {
				const chars = Math.floor(f.width * px / 7);
				const title = f.title.length <= chars ? f.title : f.title.substring(0, chars - 2) + '..';
				c.fillStyle = '#000000';
				c.fillText(title, Math.max(f.left - x0, 0) * px + 3, y + 12, f.width * px - 6);
			}

			if (f.level < root.level) {
				c.fillStyle = bg + '80';
				c.fillRect((f.left - x0) * px, y, f.width * px, 15);
			}
		}
	}

	for (let h = 0; h < levels.length; h++) {
		const y = inverted ? h * 16 : canvasHeight - (h + 1) * 16;
		const frames = levels[h];
		for (let i = 0; i < frames.length; i++) {
			drawFrame(frames[i], y);
		}
	}

	const total = totalMarked();
	updateZoomDisplay();
	return total;
}

function unpack(cpool) {
	for (let i = 1; i < cpool.length; i++) {
		cpool[i] = cpool[i - 1].substring(0, cpool[i].charCodeAt(0) - 32) + cpool[i].substring(1);
	}
}

// ── Search ────────────────────────────────────────────────────────────────────

let useRegex = false;
let caseSensitive = false;
let filterMode = false;
let filterHistory = [];

function updateFilterChips() {
	const el = document.getElementById('filtchips');
	el.innerHTML = '';
	filterHistory.forEach(function(term, i) {
		if (i > 0) {
			const sep = document.createElement('span');
			sep.className = 'filt-chip-and';
			sep.textContent = 'AND';
			el.appendChild(sep);
		}
		const chip = document.createElement('span');
		chip.className = 'filt-chip';
		chip.textContent = term;
		chip.title = 'Applied filter: ' + term;
		el.appendChild(chip);
	});
	el.classList.toggle('visible', filterHistory.length > 0);
}

// Recent search history (persisted in localStorage)
let searchHistory = [];
try { searchHistory = JSON.parse(localStorage.getItem('flame-search-history') || '[]'); } catch (e) {}

function addToHistory(term) {
	if (!term) return;
	searchHistory = [term, ...searchHistory.filter(h => h !== term)].slice(0, 10);
	try { localStorage.setItem('flame-search-history', JSON.stringify(searchHistory)); } catch (e) {}
}

function showRecentDropdown() {
	const dd = document.getElementById('recentDropdown');
	dd.innerHTML = '';
	if (!searchHistory.length) { dd.style.display = 'none'; return; }
	for (const term of searchHistory) {
		const btn = document.createElement('button');
		btn.textContent = term;
		btn.onclick = function(e) {
			e.stopPropagation();
			const inp = document.getElementById('searchinput');
			inp.value = term;
			inp.focus();
			search(term);
			dd.style.display = 'none';
		};
		dd.appendChild(btn);
	}
	dd.style.display = 'block';
}

function openSearch() {
	document.getElementById('searchbar').classList.add('open');
	document.getElementById('search').classList.add('active');
	const inp = document.getElementById('searchinput');
	inp.focus();
	inp.select();
}

function closeSearch() {
	document.getElementById('searchbar').classList.remove('open');
	document.getElementById('search').classList.remove('active');
	document.getElementById('searchinput').value = '';
	document.getElementById('recentDropdown').style.display = 'none';
	document.getElementById('matchpct').style.display = 'none';
	search('');
}

function search(r) {
	let pat;
	if (r) {
		try {
			const flags = caseSensitive ? '' : 'i';
			const expr = useRegex ? r : r.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
			pat = RegExp(expr, flags);
		} catch (e) {
			pat = undefined; // invalid regex — skip highlight
		}
	}
	pattern = pat;
	const matched = render(root, nav = []);
	navIndex = -1;
	const matchpct = document.getElementById('matchpct');
	if (r) {
		matchval = pct(matched, root.width) + '% matched';
		matchpct.textContent = matchval;
		matchpct.style.display = '';
	} else {
		matchval = '';
		matchpct.style.display = 'none';
	}
}

// ── Event wiring ──────────────────────────────────────────────────────────────

canvas.onmousemove = function() {
	if (selStart) return;
	const h = Math.floor((inverted ? event.offsetY : (canvasHeight - event.offsetY)) / 16);
	if (h >= 0 && h < levels.length) {
		const f = findFrame(levels[h], event.offsetX / px + x0Rendered);
		if (f) {
			if (f !== root) getSelection().removeAllRanges();
			hl.style.left = (Math.max(f.left - x0Rendered, 0) * px + canvas.offsetLeft) + 'px';
			hl.style.width = (Math.min(f.width, root.width / zoomFactor) * px) + 'px';
			hl.style.top = ((inverted ? h * 16 : canvasHeight - (h + 1) * 16) + canvas.offsetTop) + 'px';
			hl.firstChild.textContent = f.title;
			hl.style.display = 'block';
			canvas.title = f.title + '\n(' + samples(f.width) + f.details + ', ' + pct(f.width, levels[0][0].width) + '%)';
			const canRemove = (event.altKey || event.ctrlKey) && h >= root.level && h > 0;
			canvas.style.cursor = canRemove ? 'no-drop' : 'pointer';
			currentFrame = f;
			canvas.onclick = function() {
				if ((event.altKey || event.ctrlKey) && h >= root.level && h > 0) {
					removeStack(f.left, f.width);
					root.width > f.width ? render(root) : render();
				} else if (f !== root) {
					render(f);
				}
				canvas.onmousemove();
			};
			status.textContent = 'Function: ' + canvas.title;
			status.style.display = 'inline-block';
			return;
		}
	}
	canvas.onmouseout();
}

canvas.onmouseout = function() {
	hl.style.display = 'none';
	status.style.display = 'none';
	canvas.title = '';
	canvas.style.cursor = '';
	canvas.onclick = null;
	currentFrame = null;
}

canvas.ondblclick = function() {
	getSelection().selectAllChildren(hl);
}

// Zoom via Ctrl/Cmd+scroll; pan horizontally when zoomed via scroll or Shift+scroll
canvas.addEventListener('wheel', function(e) {
	if (e.ctrlKey || e.metaKey) {
		e.preventDefault();
		zoomAt(e.deltaY < 0 ? 1.25 : 0.8, e.offsetX);
	} else if (zoomFactor > 1) {
		const delta = Math.abs(e.deltaX) > Math.abs(e.deltaY) ? e.deltaX : e.shiftKey ? e.deltaY : 0;
		if (delta) {
			e.preventDefault();
			panSamples += delta / px;
			clampPan();
			render(root);
		}
	}
}, {passive: false});

// Multi-touch pinch-to-zoom
let touchData = null;
canvas.addEventListener('touchstart', function(e) {
	if (e.touches.length === 2) {
		e.preventDefault();
		const t0 = e.touches[0], t1 = e.touches[1];
		const dist = Math.hypot(t0.clientX - t1.clientX, t0.clientY - t1.clientY);
		const rect = canvas.getBoundingClientRect();
		const midX = (t0.clientX + t1.clientX) / 2 - rect.left;
		touchData = {dist, zoom: zoomFactor, midSample: midX / px + x0Rendered, midX};
	}
}, {passive: false});
canvas.addEventListener('touchmove', function(e) {
	if (e.touches.length === 2 && touchData) {
		e.preventDefault();
		const t0 = e.touches[0], t1 = e.touches[1];
		const dist = Math.hypot(t0.clientX - t1.clientX, t0.clientY - t1.clientY);
		const rect = canvas.getBoundingClientRect();
		const midX = (t0.clientX + t1.clientX) / 2 - rect.left;
		zoomFactor = Math.max(1, touchData.zoom * dist / touchData.dist);
		if (zoomFactor === 1) {
			panSamples = 0;
		} else {
			panSamples = touchData.midSample - root.left - midX / (canvasWidth * zoomFactor / root.width);
			clampPan();
		}
		render(root);
	}
}, {passive: false});
canvas.addEventListener('touchend', function() { touchData = null; }, {passive: true});

// Shift+drag to select a zoom area
const selbox = document.getElementById('selbox');
let selStart = null;

// Track the currently hovered frame so keydown/keyup can update the cursor
let currentFrame = null;

document.addEventListener('keydown', function(e) {
	if (e.key === 'Shift' && !selStart) canvas.style.cursor = 'crosshair';
	if ((e.key === 'Control' || e.key === 'Alt') && currentFrame) canvas.style.cursor = 'no-drop';
});
document.addEventListener('keyup', function(e) {
	if (e.key === 'Shift' && !selStart) canvas.style.cursor = currentFrame ? (event.ctrlKey || event.altKey ? 'no-drop' : 'pointer') : '';
	if ((e.key === 'Control' || e.key === 'Alt') && currentFrame) canvas.style.cursor = 'pointer';
});

canvas.addEventListener('mousedown', function(e) {
	if (e.altKey || e.ctrlKey || e.metaKey) return;
	if (e.shiftKey) {
		selStart = {x: e.offsetX};
		const rect = canvas.getBoundingClientRect();
		selbox.style.top = (rect.top + window.scrollY) + 'px';
		selbox.style.height = rect.height + 'px';
		selbox.style.left = (rect.left + window.scrollX + e.offsetX) + 'px';
		selbox.style.width = '0px';
		selbox.style.display = 'block';
		canvas.onclick = null;
	}
});

canvas.addEventListener('mousemove', function(e) {
	if (selStart) {
		const rect = canvas.getBoundingClientRect();
		const x0 = selStart.x, x1 = e.offsetX;
		selbox.style.left = (rect.left + window.scrollX + Math.min(x0, x1)) + 'px';
		selbox.style.width = Math.abs(x1 - x0) + 'px';
	}
});

document.addEventListener('mouseup', function(e) {
	if (selStart) {
		selbox.style.display = 'none';
		const canvasX = e.clientX - canvas.getBoundingClientRect().left;
		const x0 = Math.min(selStart.x, canvasX);
		const x1 = Math.max(selStart.x, canvasX);
		selStart = null;
		canvas.style.cursor = e.shiftKey ? 'crosshair' : '';
		if (x1 - x0 > 4) {
			const sampleL = x0Rendered + x0 / px;
			const sampleR = x0Rendered + x1 / px;
			zoomFactor = root.width / (sampleR - sampleL);
			panSamples = sampleL - root.left;
			clampPan();
			render(root);
		}
	}
});

// Close recent dropdown on any outside click
document.addEventListener('click', function() {
	document.getElementById('recentDropdown').style.display = 'none';
});

document.getElementById('inverted').onclick = function() {
	inverted = !inverted;
	render();
}

document.getElementById('search').onclick = function() {
	if (document.getElementById('searchbar').classList.contains('open')) {
		closeSearch();
	} else {
		openSearch();
	}
};

document.getElementById('recentBtn').onclick = function(e) {
	e.stopPropagation();
	showRecentDropdown();
};

document.getElementById('searchinput').oninput = function() {
	search(this.value);
};

document.getElementById('searchinput').onkeydown = function(e) {
	if (e.key === 'Escape') {
		closeSearch();
	} else if (e.key === 'Enter') {
		if (filterMode && nav.length > 0) {
			addToHistory(this.value);
			keepOnlyMatched(this.value);
			// Stay in search — user may want to refine and filter again
		} else if (this.value) {
			addToHistory(this.value);
		}
		e.preventDefault();
	}
	e.stopPropagation();
};

document.getElementById('toggleCase').onclick = function() {
	caseSensitive = !caseSensitive;
	this.classList.toggle('active', caseSensitive);
	search(document.getElementById('searchinput').value);
};

document.getElementById('toggleRegex').onclick = function() {
	useRegex = !useRegex;
	this.classList.toggle('active', useRegex);
	document.getElementById('searchinput').placeholder = useRegex ? 'Regex…' : 'Search…';
	search(document.getElementById('searchinput').value);
};

document.getElementById('modeHL').onclick = function() {
	filterMode = false;
	restoreStacks()
	this.classList.add('active');
	document.getElementById('modeFilt').classList.remove('active');

};

document.getElementById('modeFilt').onclick = function() {
	filterMode = true;
	this.classList.add('active');
	document.getElementById('modeHL').classList.remove('active');
	// Apply immediately if there are current matches
	const term = document.getElementById('searchinput').value;
	if (nav && nav.length > 0) {
		addToHistory(term);
		keepOnlyMatched(term);
	}
};

document.getElementById('restoreBtn').onclick = restoreStacks;

document.getElementById('zoomreset').onclick = function() {
	zoomFactor = 1; panSamples = 0;
	render(root);
};

document.getElementById('zoomin').onclick = function() {
	zoomAt(1.5, canvasWidth / 2);
}

document.getElementById('zoomout').onclick = function() {
	zoomAt(1 / 1.5, canvasWidth / 2);
}

document.getElementById('darkmode').onclick = function() {
	const theme = document.documentElement.classList.toggle('dark') ? 'dark' : 'light';
	try { localStorage.setItem('flame-theme', theme); } catch (ignored) {}
	render(root);
}

const btnInfo = document.getElementById('info');
const legend = document.getElementById('legend');

btnInfo.onmouseover = function() {
	legend.style.left = (btnInfo.offsetLeft + 24) + 'px';
	legend.style.top = (btnInfo.offsetTop + 24) + 'px';
	legend.style.display = 'block';
}

btnInfo.onmouseout = function() {
	legend.style.display = 'none';
}

window.onkeydown = function(event) {
	if ((event.ctrlKey || event.metaKey) && event.key === 'f') {
		event.preventDefault();
		openSearch();
		return false;
	} else if (event.key === 'Escape') {
		closeSearch();
	} else if ((event.key === 'n' || event.key === 'N') && nav.length > 0) {
		navIndex = (navIndex + (event.shiftKey ? nav.length - 1 : 1)) % nav.length;
		zoomFactor = 1; panSamples = 0;
		render(nav[navIndex]);
		document.getElementById('matchpct').textContent = matchval + ' (' + (navIndex + 1) + ' of ' + nav.length + ')';
		window.scroll(0, inverted ? root.level * 16 : canvasHeight - (root.level + 1) * 16);
		canvas.onmousemove();
		return false;
	} else if (event.key === 'i') {
		canvas.onmouseout();
		document.getElementById('inverted').onclick();
		return false;
	} else if (event.key === 'd') {
		document.getElementById('darkmode').onclick();
		return false;
	} else if (event.key === '0') {
		canvas.onmouseout();
		zoomFactor = 1; panSamples = 0;
		root = levels[0][0];
		closeSearch();
		return false;
	} else if (event.key === '=' || event.key === '+') {
		zoomAt(1.5, canvasWidth / 2);
		return false;
	} else if (event.key === '-') {
		zoomAt(1 / 1.5, canvasWidth / 2);
		return false;
	}
}
