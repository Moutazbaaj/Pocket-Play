#pragma once

namespace {
constexpr char kTransferPage[] = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Pocket Play File Transfer</title>
<style>
  :root { color-scheme: dark; font: 16px system-ui, sans-serif; background: #101413; color: #eef4ed; }
  * { box-sizing: border-box; }
  body { margin: 0; }
  main { max-width: 42rem; margin: auto; padding: 2rem 1rem; }
  h1 { margin: 0 0 1.5rem; font-size: clamp(1.7rem, 5vw, 2.4rem); }
  h2 { margin: 0 0 .75rem; font-size: 1.25rem; }
  p { line-height: 1.5; color: #c6d1c9; }
  section { padding: 1.2rem; margin-bottom: 1rem; background: #18201d; border: 1px solid #39483e; border-radius: .7rem; }
  label { display: block; margin: .75rem 0 .35rem; }
  #drop { padding: 1rem; margin-bottom: .75rem; border: 1px dashed #7a9985; border-radius: .4rem; }
  #drop.over { background: #263d30; }
  input { font: inherit; max-width: 100%; }
  button { font: inherit; font-weight: 650; color: #15241b; background: #a7e4b7; border: 0; border-radius: .4rem; padding: .6rem .9rem; }
  button:disabled { opacity: .55; }
  #restart { color: #eef4ed; background: #2e3a32; }
  progress { width: 100%; margin-top: .75rem; accent-color: #a7e4b7; }
  output { display: block; margin-top: .6rem; overflow-wrap: anywhere; white-space: pre-line; }
</style>
</head>
<body>
<main>
  <h1>Pocket Play File Transfer</h1>
  <section>
    <h2>Upload to microSD</h2>
    <p>Games, MP3 and WAV tracks, and photos go to their matching folders. Files with the same name stay on the card.</p>
    <div id="drop">
      <label for="files">Drop files here or choose files</label>
      <input id="files" type="file" multiple accept=".gb,.gbc,.gba,.nes,.sms,.gg,.md,.gen,.smd,.wad,.mp3,.wav,.jpg,.jpeg,.png,.bmp,.gif,.psd">
    </div>
    <button id="upload" type="button" disabled>Upload</button>
    <progress id="progress" value="0" max="1" hidden></progress>
    <output id="result" aria-live="polite"></output>
  </section>
  <section>
    <h2>Applications</h2>
    <p>Install a Pocket Play app or game on microSD, or save the package for later.</p>
    <label for="appPackage">Pocket Play app package (.ppapp)</label>
    <input id="appPackage" type="file" accept=".ppapp" disabled>
    <button id="installApp" type="button" disabled>Install application</button>
    <button id="saveApp" type="button" disabled>Save to microSD</button>
    <progress id="appProgress" value="0" max="1" hidden></progress>
    <output id="appResult" aria-live="polite"></output>
  </section>
  <section>
    <h2>Firmware update</h2>
    <p>Choose firmware for your hardware. Install it now or save it to microSD to install later from About → Update Software.</p>
    <label for="firmware">Pocket Play firmware image (.ino.bin)</label>
    <input id="firmware" type="file" accept=".bin" disabled>
    <button id="install" type="button" disabled>Install firmware</button>
    <button id="saveFirmware" type="button" disabled>Save to microSD</button>
    <progress id="firmwareProgress" value="0" max="1" hidden></progress>
    <output id="firmwareResult" aria-live="polite"></output>
  </section>
  <button id="restart" type="button" disabled>Restart console</button>
  <output id="sessionResult" aria-live="polite"></output>
</main>
<script>
const byId = id => document.getElementById(id);
const files = byId('files'), upload = byId('upload'), drop = byId('drop');
const result = byId('result'), progress = byId('progress'), restart = byId('restart');
const sessionResult = byId('sessionResult');
const appPackage = byId('appPackage'), installApp = byId('installApp'), saveApp = byId('saveApp');
const appProgress = byId('appProgress'), appResult = byId('appResult');
const firmware = byId('firmware'), install = byId('install'), saveFirmware = byId('saveFirmware');
const firmwareResult = byId('firmwareResult'), firmwareProgress = byId('firmwareProgress');
let queue = [], token = '', busy = false, cardReady = false, otaStatus = '';

function refreshButtons() {
  upload.disabled = busy || !token || !cardReady || !queue.length;
  restart.disabled = busy || !token;
  files.disabled = busy || !cardReady;
  appPackage.disabled = busy || !token || !cardReady;
  installApp.disabled = saveApp.disabled = appPackage.disabled || !appPackage.files.length;
  firmware.disabled = busy || !token || (otaStatus !== 'ready' && !cardReady);
  install.disabled = firmware.disabled || otaStatus !== 'ready' || !firmware.files.length;
  saveFirmware.disabled = firmware.disabled || !cardReady || !firmware.files.length;
}

function choose(selection) {
  if (busy || !cardReady) return;
  queue = Array.from(selection);
  result.textContent = queue.length ? queue.length + ' selected' : '';
  progress.hidden = true;
  refreshButtons();
}

files.onchange = () => choose(files.files);
drop.ondragover = event => {
  event.preventDefault();
  if (!busy && cardReady) drop.classList.add('over');
};
drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = event => {
  event.preventDefault();
  drop.classList.remove('over');
  if (busy) return;
  files.value = '';
  choose(event.dataTransfer.files);
};

function sendFile(file, route, bar) {
  return new Promise((resolve, reject) => {
    const request = new XMLHttpRequest();
    request.open('POST', route);
    request.setRequestHeader('X-Pocket-Token', token);
    request.setRequestHeader('X-Pocket-File', encodeURIComponent(file.name));
    request.upload.onprogress = event => {
      if (event.lengthComputable) bar.value = event.loaded / event.total;
    };
    request.onload = () => request.status === 201 ? resolve(request.responseText)
      : reject(new Error(request.responseText || 'Upload failed.'));
    request.onerror = () => reject(new Error('Connection lost. Reconnect and try again.'));
    request.send(file);
  });
}

upload.onclick = async () => {
  busy = true;
  refreshButtons();
  progress.hidden = false;
  const failed = [];
  let uploaded = 0;
  for (const [index, file] of queue.entries()) {
    progress.value = 0;
    result.textContent = 'Uploading ' + (index + 1) + ' of ' + queue.length + ': ' + file.name;
    try { await sendFile(file, '/upload', progress); uploaded++; }
    catch (error) { failed.push(file.name + ': ' + error.message); }
  }
  result.textContent = [uploaded + ' uploaded.', ...failed].join('\n');
  progress.hidden = true;
  queue = [];
  files.value = '';
  busy = false;
  refreshButtons();
};

appPackage.onchange = refreshButtons;
async function transferApp(installNow) {
  const file = appPackage.files[0];
  if (!file || !/^[a-z0-9][a-z0-9_-]{0,31}\.ppapp$/.test(file.name)) {
    appResult.textContent = 'Use a lowercase app ID followed by .ppapp.';
    return;
  }
  if (installNow && !confirm('Install ' + file.name + ' on Pocket Play?')) return;
  busy = true;
  refreshButtons();
  appProgress.value = 0;
  appProgress.hidden = false;
  appResult.textContent = installNow ? 'Installing application…' : 'Saving package to microSD…';
  try {
    const path = await sendFile(file, installNow ? '/application' : '/app-package', appProgress);
    appResult.textContent = installNow
      ? 'Installed to ' + path + '. Restart the console and open Applications.'
      : 'Saved to ' + path + '. Install from About → Install Application.';
    appPackage.value = '';
  } catch (error) {
    appResult.textContent = error.message;
  }
  appProgress.hidden = true;
  busy = false;
  refreshButtons();
}
installApp.onclick = () => transferApp(true);
saveApp.onclick = () => transferApp(false);

firmware.onchange = refreshButtons;
async function transferFirmware(toCard) {
  const file = firmware.files[0];
  if (!file || !file.name.toLowerCase().endsWith('.ino.bin')) {
    firmwareResult.textContent = 'Choose a Pocket Play .ino.bin app image.';
    return;
  }
  if (!toCard && !confirm('Install ' + file.name + ' on Pocket Play?')) return;
  busy = true;
  refreshButtons();
  firmwareProgress.value = 0;
  firmwareProgress.hidden = false;
  firmwareResult.textContent = toCard ? 'Saving firmware to microSD…' : 'Uploading and checking firmware…';
  try {
    const message = await sendFile(file, toCard ? '/upload' : '/firmware', firmwareProgress);
    firmwareResult.textContent = toCard ? 'Saved to ' + message + '. Install from About → Update Software.' : message;
    if (!toCard) otaStatus = 'staged';
    firmware.value = '';
  } catch (error) {
    firmwareResult.textContent = error.message;
  }
  firmwareProgress.hidden = true;
  busy = false;
  refreshButtons();
}
install.onclick = () => transferFirmware(false);
saveFirmware.onclick = () => transferFirmware(true);

restart.onclick = async () => {
  busy = true;
  refreshButtons();
  try {
    const response = await fetch('/restart', {method: 'POST', headers: {'X-Pocket-Token': token}});
    sessionResult.textContent = await response.text();
    if (response.ok) token = '';
  } catch {
    sessionResult.textContent = 'Connection lost. Check Pocket Play.';
  }
  busy = false;
  refreshButtons();
};

fetch('/session').then(response => {
  if (!response.ok) throw new Error();
  cardReady = response.headers.get('X-Pocket-SD') === 'ready';
  otaStatus = response.headers.get('X-Pocket-OTA');
  if (!cardReady) result.textContent = 'Insert a microSD card and restart transfer mode to upload files.';
  if (otaStatus === 'unavailable') firmwareResult.textContent = 'This console needs a USB firmware update before it can install app images.';
  if (otaStatus === 'staged') firmwareResult.textContent = 'Firmware ready. Restart console to apply it.';
  return response.text();
}).then(text => {
  token = text;
  refreshButtons();
}).catch(() => {
  sessionResult.textContent = 'Join the Wi-Fi network shown on Pocket Play, then reload this page.';
});
</script>
</body>
</html>)HTML";
}
