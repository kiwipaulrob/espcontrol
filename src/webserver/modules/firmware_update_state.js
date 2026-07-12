// ── Firmware Update State ─────────────────────────────────────────────
// @web-module-requires: state, firmware_metadata, firmware_version_state

var firmwareInstallRefreshTimer = null;
var firmwareInstallRefreshUntil = 0;
var firmwareWebOtaFallbackTimer = null;
var FIRMWARE_WEB_OTA_FALLBACK_DELAY_MS = 12000;

function firmwareUpdateAvailable() {
  return state.firmwareUpdateState === "UPDATE AVAILABLE" &&
    isSpecificFirmwareVersion(state.firmwareLatestVersion);
}

function publicFirmwareInstallAvailable() {
  return publicFirmwareReleaseKnown() && !installedFirmwareMatchesPublicRelease();
}

function latestFirmwareInfo() {
  return findFirmwareVersionInfo(state.firmwareLatestVersion) || latestFirmwareInfoFromState();
}

function latestFirmwareInstallAvailable() {
  var info = latestFirmwareInfo();
  return state.firmwareInstallControlsSupported === true &&
    !!info &&
    isSpecificFirmwareVersion(info.latest_version) &&
    !installedFirmwareMatchesPublicRelease();
}

function latestFirmwareInstallAction() {
  if (!latestFirmwareInstallAvailable()) return "check";
  return firmwareUpdateAvailable() ? "install" : "check_then_install";
}

function latestFirmwareInfoFromState() {
  if (!isSpecificFirmwareVersion(state.firmwareLatestVersion)) return null;
  return {
    latest_version: state.firmwareLatestVersion,
    release_url: state.firmwareReleaseUrl,
    ota_url: state.firmwareOtaUrl,
    ota_filename: state.firmwareOtaFilename || (DEVICE_ID + ".ota.bin"),
    ota_md5: state.firmwareOtaMd5,
  };
}

function findFirmwareVersionInfo(version) {
  version = String(version || "").trim();
  if (!version) return null;
  for (var i = 0; i < state.firmwareVersionOptions.length; i++) {
    var info = state.firmwareVersionOptions[i];
    if (firmwareVersionsSame(info.latest_version, version)) return info;
  }
  var latest = latestFirmwareInfoFromState();
  if (latest && firmwareVersionsSame(latest.latest_version, version)) return latest;
  return null;
}

function selectedFirmwareInfo() {
  return findFirmwareVersionInfo(state.firmwareSelectedVersion) ||
    (state.firmwareVersionOptions.length ? state.firmwareVersionOptions[0] : null) ||
    latestFirmwareInfoFromState();
}

function previousFirmwareInfos() {
  return state.firmwareVersionOptions.filter(function (info) {
    var version = info && info.latest_version;
    return isSpecificFirmwareVersion(version) &&
      !firmwareVersionsSame(version, state.firmwareLatestVersion) &&
      !firmwareVersionsSame(version, state.firmwareVersion);
  });
}

function selectedPreviousFirmwareInfo() {
  var options = previousFirmwareInfos();
  for (var i = 0; i < options.length; i++) {
    if (firmwareVersionsSame(options[i].latest_version, state.firmwareSelectedVersion)) {
      return options[i];
    }
  }
  return options.length ? options[0] : null;
}

function previousFirmwareInstallAvailable() {
  var info = selectedPreviousFirmwareInfo();
  return state.firmwareInstallControlsSupported === true &&
    !!info &&
    !firmwareVersionsSame(info.latest_version, state.firmwareVersion);
}

function firmwareVersionSelectorVisible() {
  return state.firmwareVersionIndexLoaded && previousFirmwareInfos().length > 0;
}

function syncFirmwareVersionSelect() {
  if (!els.fwVersionSelect) return;
  var options = previousFirmwareInfos();
  els.fwVersionSelect.innerHTML = "";
  if (!options.length) {
    state.firmwareSelectedVersion = "";
    syncPreviousFirmwareUi();
    return;
  }
  state.firmwareSelectedVersion = selectedPreviousFirmwareInfo().latest_version;
  for (var i = 0; i < options.length; i++) {
    var info = options[i];
    var option = document.createElement("option");
    option.value = info.latest_version;
    option.textContent = info.latest_version;
    els.fwVersionSelect.appendChild(option);
  }
  els.fwVersionSelect.value = state.firmwareSelectedVersion;
  syncPreviousFirmwareUi();
}

function syncPreviousFirmwareUi() {
  var show = firmwareUpdateControlsVisible() && firmwareVersionSelectorVisible();
  if (els.fwPreviousPanel) els.fwPreviousPanel.style.display = show ? "" : "none";
  var busy = state.firmwareUpdateState === "INSTALLING" || state.firmwareChecking;
  if (els.fwVersionSelect) els.fwVersionSelect.disabled = busy || !show;
  if (els.fwPreviousInstallBtn) {
    els.fwPreviousInstallBtn.disabled = busy || !show || !previousFirmwareInstallAvailable();
    els.fwPreviousInstallBtn.className = "sp-fw-btn" + (busy ? " sp-fw-btn-busy" : "");
    els.fwPreviousInstallBtn.textContent = state.firmwareUpdateState === "INSTALLING" ? "Installing…" : "Install";
  }
}

function setPublicFirmwareInfo(info) {
  if (!info) return false;
  var latest = String(info.latest_version || "").trim();
  if (!isSpecificFirmwareVersion(latest)) return false;
  state.firmwareLatestVersion = latest;
  if (info.release_url) state.firmwareReleaseUrl = String(info.release_url).trim();
  if (info.ota_url) state.firmwareOtaUrl = String(info.ota_url).trim();
  if (info.ota_filename) state.firmwareOtaFilename = String(info.ota_filename).trim();
  if (info.ota_md5) state.firmwareOtaMd5 = String(info.ota_md5).trim();
  if (state.firmwareUpdateState === "NO UPDATE" &&
      !isSpecificFirmwareVersion(state.firmwareVersion)) {
    setFirmwareVersion(latest);
  }
  syncFirmwareVersionSelect();
  renderFirmwareUpdateStatus();
  return true;
}

function setPublicFirmwareVersions(infos) {
  if (!Array.isArray(infos) || !infos.length) return false;
  state.firmwareVersionOptions = infos;
  state.firmwareVersionIndexLoaded = true;
  if (!state.firmwareSelectedVersion || !findFirmwareVersionInfo(state.firmwareSelectedVersion)) {
    state.firmwareSelectedVersion = infos[0].latest_version;
  }
  setPublicFirmwareInfo(infos[0]);
  syncFirmwareVersionSelect();
  renderFirmwareUpdateStatus();
  return true;
}

function publicFirmwareReleaseKnown() {
  return isSpecificFirmwareVersion(state.firmwareLatestVersion);
}

function installedFirmwareMatchesPublicRelease() {
  return publicFirmwareReleaseKnown() &&
    isSpecificFirmwareVersion(state.firmwareVersion) &&
    firmwareVersionsSame(state.firmwareVersion, state.firmwareLatestVersion);
}

function firmwareUpdateControlsVisible() {
  return state.firmwareUpdateControlsSupported === true;
}

function syncFirmwareUpdateUi() {
  var show = firmwareUpdateControlsVisible();
  if (els.fwActions) els.fwActions.style.display = show ? "" : "none";
  if (els.fwStatus) els.fwStatus.style.display = show ? "" : "none";
  if (els.autoUpdatePanel) els.autoUpdatePanel.style.display = show ? "" : "none";
  if (els.autoUpdateBadge) els.autoUpdateBadge.classList.toggle("sp-hidden", !state.autoUpdate);
  if (els.setAutoUpdateRow) els.setAutoUpdateRow.style.display = show ? "" : "none";
  if (els.updateFreqWrap) {
    els.updateFreqWrap.style.display = show && state.autoUpdate ? "" : "none";
  }
  syncPreviousFirmwareUi();
}

function renderFirmwareUpdateStatus() {
  if (!els.fwStatus) return;
  var cls = "sp-fw-status";
  var status = "";
  var inlineStatus = "";
  if (els.fwLatestVersion) {
    if (publicFirmwareReleaseKnown()) {
      els.fwLatestVersion.textContent = state.firmwareLatestVersion;
    } else if (state.firmwareChecking) {
      els.fwLatestVersion.textContent = "Checking\u2026";
    } else {
      els.fwLatestVersion.textContent = "Not checked";
    }
  }
  if (state.firmwareUpdateState === "INSTALLING") {
    status = state.firmwareInstallStatus || "Installing update\u2026";
    cls += " sp-update-installing";
  } else if (state.firmwareInstallError) {
    status = escHtml(state.firmwareInstallError);
    cls += " sp-update-error";
  } else if (state.firmwareUpdateState === "NO UPDATE") {
    if (installedFirmwareMatchesPublicRelease() || !latestFirmwareInstallAvailable()) {
      inlineStatus = "Up to date";
    }
  } else if (state.firmwareChecking) {
    status = "Checking for an update\u2026";
  }
  els.fwStatus.className = cls;
  els.fwStatus.innerHTML = status;
  if (els.fwInlineStatus) {
    els.fwInlineStatus.className = "sp-fw-inline-status" + (inlineStatus ? " sp-visible" : "");
    els.fwInlineStatus.textContent = inlineStatus;
  }
  if (els.fwCheckBtn) {
    var isBusy = state.firmwareUpdateState === "INSTALLING" || state.firmwareChecking;
    els.fwCheckBtn.className = "sp-fw-btn" + (isBusy ? " sp-fw-btn-busy" : "");
    if (state.firmwareUpdateState === "INSTALLING") {
      els.fwCheckBtn.disabled = true;
      els.fwCheckBtn.textContent = "Installing\u2026";
    } else if (latestFirmwareInstallAvailable()) {
      els.fwCheckBtn.disabled = false;
      els.fwCheckBtn.textContent = "Install Update";
    } else {
      els.fwCheckBtn.disabled = state.firmwareChecking;
      els.fwCheckBtn.textContent = state.firmwareChecking ? "Checking\u2026" : "Check for Update";
    }
  }
  syncFirmwareUpdateUi();
}

function setFirmwareUpdateInfo(d) {
  state.firmwareUpdateControlsSupported = true;
  state.firmwareInstallControlsSupported = true;
  var latest = d.latest_version || d.value || "";
  var updateState = String(d.state || state.firmwareUpdateState || "").trim().toUpperCase();
  if (d.current_version) setFirmwareVersion(d.current_version);
  if (latest) state.firmwareLatestVersion = String(latest).trim();
  var installWindowActive = !!state.firmwareInstallTargetVersion &&
    Date.now() < firmwareInstallRefreshUntil;
  if (state.firmwareInstallPostPending) {
    if (installWindowActive && updateState === "UPDATE AVAILABLE") {
      state.firmwareInstallPostPending = false;
      clearFirmwareWebOtaFallback();
      state.firmwareInstallStatus = "Installing update\u2026";
      postFirmwareUpdateInstall();
      updateState = "INSTALLING";
    } else if (!installWindowActive || (updateState === "NO UPDATE" && !publicFirmwareInstallAvailable())) {
      state.firmwareInstallPostPending = false;
    }
  }
  if (installWindowActive && updateState === "UPDATE AVAILABLE") {
    updateState = "INSTALLING";
  }
  state.firmwareUpdateState = updateState;
  if (state.firmwareUpdateState) state.firmwareInstallError = "";
  state.firmwareReleaseUrl = d.release_url || state.firmwareReleaseUrl || "";
  if (state.firmwareUpdateState === "NO UPDATE" &&
      !isSpecificFirmwareVersion(state.firmwareVersion) &&
      isSpecificFirmwareVersion(state.firmwareLatestVersion)) {
    setFirmwareVersion(state.firmwareLatestVersion);
  }
  if (state.firmwareUpdateState) state.firmwareChecking = false;
  if (state.firmwareUpdateState === "INSTALLING") {
    startFirmwareInstallRefresh();
  } else {
    stopFirmwareInstallRefreshIfComplete();
  }
  renderFirmwareUpdateStatus();
}

function firmwareVersionMatches(version, expected) {
  return String(version == null ? "" : version).trim() ===
    String(expected == null ? "" : expected).trim();
}

function stopFirmwareInstallRefresh() {
  if (firmwareInstallRefreshTimer) clearTimeout(firmwareInstallRefreshTimer);
  firmwareInstallRefreshTimer = null;
  firmwareInstallRefreshUntil = 0;
  clearFirmwareWebOtaFallback();
  state.firmwareInstallTargetVersion = "";
  state.firmwareInstallPostPending = false;
  state.firmwareInstallStatus = "";
}

function stopFirmwareInstallRefreshIfComplete() {
  var target = state.firmwareInstallTargetVersion;
  if (!target || state.firmwareUpdateState !== "NO UPDATE") return false;
  if (isSpecificFirmwareVersion(target) && !firmwareVersionMatches(state.firmwareVersion, target)) {
    setFirmwareVersion(target);
  }
  stopFirmwareInstallRefresh();
  return true;
}

function pollFirmwareInstallRefresh() {
  firmwareInstallRefreshTimer = null;
  refreshFirmwareVersion();
  if (stopFirmwareInstallRefreshIfComplete()) return;
  if (Date.now() >= firmwareInstallRefreshUntil) {
    stopFirmwareInstallRefresh();
    return;
  }
  firmwareInstallRefreshTimer = setTimeout(pollFirmwareInstallRefresh, 5000);
}

function startFirmwareInstallRefresh() {
  if (!state.firmwareInstallTargetVersion && isSpecificFirmwareVersion(state.firmwareLatestVersion)) {
    state.firmwareInstallTargetVersion = state.firmwareLatestVersion;
  }
  firmwareInstallRefreshUntil = Date.now() + 180000;
  if (firmwareInstallRefreshTimer) clearTimeout(firmwareInstallRefreshTimer);
  firmwareInstallRefreshTimer = setTimeout(pollFirmwareInstallRefresh, 5000);
}

function clearFirmwareWebOtaFallback() {
  if (firmwareWebOtaFallbackTimer) clearTimeout(firmwareWebOtaFallbackTimer);
  firmwareWebOtaFallbackTimer = null;
}

function scheduleFirmwareWebOtaFallback() {
  clearFirmwareWebOtaFallback();
  firmwareWebOtaFallbackTimer = setTimeout(function () {
    firmwareWebOtaFallbackTimer = null;
    if (!state.firmwareInstallPostPending) return;
    if (firmwareUpdateAvailable()) return;
    if (!publicFirmwareInstallAvailable()) return;
    installPublicFirmwareViaWebOta(latestFirmwareInfo());
  }, FIRMWARE_WEB_OTA_FALLBACK_DELAY_MS);
}
