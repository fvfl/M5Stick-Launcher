function _(e) { return document.getElementById(e); }

function httpRequest(method, url, { async = true, body, headers = {}, onload, onerror } = {}) {
    const xhr = new XMLHttpRequest();
    if (typeof onload === "function") {
        xhr.onload = () => onload(xhr);
    }
    xhr.onerror = () => {
        if (typeof onerror === "function") {
            onerror(xhr);
        } else {
            console.error("Network error or request failure.");
        }
    };
    xhr.open(method, url, async);
    Object.keys(headers).forEach((header) => xhr.setRequestHeader(header, headers[header]));
    if (body !== undefined && body !== null) {
        xhr.send(body);
    } else {
        xhr.send();
    }
    return xhr;
}

function isNullOrEmpty(value) {
    return value === null || value === "";
}

function WifiConfig(target) {
    let wifiSsid;
    let wifiPwd;
    if (target === "usr") {
        wifiSsid = prompt("Username of access Launcher", "admin");
        wifiPwd = prompt("Password", "launcher");
    } else if (target === "ssid") {
        wifiSsid = prompt("SSID of your network", "");
        wifiPwd = prompt("Password of your network", "");
    }
    if (isNullOrEmpty(wifiSsid) || wifiPwd === null) {
        window.alert("Invalid " + target + " or password");
    } else {
        const xhr = httpRequest("GET", "/wifi?" + target + "=" + wifiSsid + "&pwd=" + wifiPwd, { async: false });
        _("status").innerHTML = xhr.responseText;
    }
}

function SDConfig() {
    const miso = prompt("MISO pin", "");
    const mosi = prompt("MOSI pin", "");
    const sck = prompt("SCK pin", "");
    const cs = prompt("CS pin", "");
    if ([miso, mosi, sck, cs].some(isNullOrEmpty)) {
        window.alert("Invalid pins");
    } else {
        const xhr = httpRequest("GET", "/sdpins?miso=" + miso + "&mosi=" + mosi + "&sck=" + sck + "&cs=" + cs, { async: false });
        _("status").innerHTML = xhr.responseText;
    }
}
function startUpdate(fileName) {
    const formdata4 = new FormData();
    formdata4.append("fileName", fileName);
    httpRequest("POST", "/UPDATE", { async: false, body: formdata4 });
}
function callOTA() {
    const formdata = new FormData();
    formdata.append("update", 1);
    httpRequest("POST", "/OTA", { async: false, body: formdata });
    _("detailsheader").innerHTML = "<h3>OTA Update</h3>";
    _("status").innerHTML = "";
    _("details").innerHTML = "";
    _("updetailsheader").innerHTML = "";
    _("updetails").innerHTML = "";
    _("OTAdetails").style.display = 'block';
    _("drop-area").style.display = 'none';
    _("fileInput").click();
}
function analyzeFile() {
    const fileInput = _('fileInput');
    const outputDiv = _('analysisOutput');
    const uploadAppBtn = _('uploadApp');
    const uploadSpiffsBtn = _('uploadSpiffs');
    outputDiv.style.display = 'none';
    uploadAppBtn.style.display = 'none';
    uploadSpiffsBtn.style.display = 'none';
    let pass = true;
    if (fileInput.files.length === 0) {
        window.alert('Please, select a file.');
        return;
    }
    if (fileInput.files[0].name.split('.').pop() !== "bin") {
        window.alert('File is not a .bin');
        return;
    }
    const file = fileInput.files[0];
    const reader = new FileReader();
    reader.onload = function (e) {
        const data = new Uint8Array(reader.result);
        let start_point = 0;
        let spiffs_offset = 0;
        let spiffs_size = 0;
        let app_size = 0;
        let spiffs = false;
        const MAX_APP = 0x470000;
        const MAX_SPIFFS = 0x100000;
        const first_slice = data.slice(0x8000, 0x8000 + 16);
        const byte0 = first_slice[0];
        const byte1 = first_slice[1];
        const byte2 = first_slice[2];
        if (byte0 === 0xaa && byte1 === 0x50 && byte2 === 0x01 && pass === true) {
            pass = false;
            start_point = 0x10000;
            for (let i = 0; i < 0xA0; i += 0x20) {
                const pos = 0x8000 + i;
                if (pos + 16 > data.length) break;
                const slice = data.slice(pos, pos + 16);
                const byte3 = slice[3];
                const byte6 = slice[6];
                if ([0x00, 0x10, 0x20].includes(byte3) && byte6 === 0x01) {
                    app_size = (slice[10] << 16) | (slice[11] << 8) | 0x00;
                    if (data.length < (app_size + 0x10000)) app_size = data.length - 0x10000;
                    if (app_size > MAX_APP) app_size = MAX_APP;
                }
                if (byte3 === 0x82) {
                    spiffs_offset = (slice[6] << 16) | (slice[7] << 8) | slice[8];
                    spiffs_size = (slice[10] << 16) | (slice[11] << 8);
                    if (data.length < spiffs_offset) spiffs = false;
                    else if (spiffs_size > MAX_SPIFFS) {
                        spiffs_size = MAX_SPIFFS;
                        spiffs = true;
                    }
                    if (spiffs && data.length < (spiffs_offset + spiffs_size)) spiffs_size = data.length - spiffs_offset;
                }
            }
        }
        else if (pass === true) {
            pass = false;
            start_point = 0x0;
            app_size = data.length;
            spiffs = false;
        }
        const appBlob = new Blob([data.slice(start_point, start_point + app_size)], { type: 'application/octet-stream' });
        const spiffsBlob = spiffs ? new Blob([data.slice(spiffs_offset, spiffs_offset + spiffs_size)], { type: 'application/octet-stream' }) : null;
        if (app_size > 0) {
            uploadAppBtn.style.display = 'inline';
            uploadAppBtn.onclick = () => uploadSlice(appBlob, app_size, file.name + '-app.bin', 0);
        }
        if (spiffs) {
            uploadSpiffsBtn.style.display = 'inline';
            _("spiffsInfo").style.display = 'block';
            uploadSpiffsBtn.onclick = () => uploadSlice(spiffsBlob, spiffs_size, file.name + '-spiffs.bin', 100);
        }
    };
    reader.readAsArrayBuffer(file);
}
function uploadSlice(blobData, c_size, fileName, comm) {
    _("updetails").innerHTML = "Preparing...";
    totalFiles = 1;
    completedFiles = 0;
    const ajax = new XMLHttpRequest();
    ajax.onload = function () {
        if (ajax.status === 200 && ajax.responseText === "OK") {
            const fileProgressDiv = document.createElement("div");
            fileProgressDiv.innerHTML = `<p>Updating...</p><p><progress id="otaprb" value="0" max="100" style="width:100%;"></progress></p>`;
            _("updetails").appendChild(fileProgressDiv);
            const formdata2 = new FormData();
            formdata2.append("file1", blobData, fileName);
            const ajax2 = new XMLHttpRequest();
            ajax2.open("POST", "/OTAFILE");
            ajax2.upload.addEventListener("progress", function (event) {
                const p = (event.loaded / event.total) * 100;
                _("otaprb").value = Math.round(p);
            }, false);
            ajax2.addEventListener("load", function () { _("status").innerHTML = "Instalation Complete, Restart your device!"; }, false);
            ajax2.addEventListener("error", function () { _("status").innerHTML = "Upload Failed"; }, false);
            ajax2.addEventListener("abort", function () { _("status").innerHTML = "Upload Aborted"; }, false);
            ajax2.send(formdata2);
        }
    };
    ajax.onerror = function () {
        console.error("Initial OTA request failed.");
    };
    const formdata = new FormData();
    formdata.append("command", comm);
    formdata.append("size", c_size);
    ajax.open("POST", "/OTA", true);
    ajax.send(formdata);
}
function logoutButton() {
    httpRequest("GET", "/logout");
    setTimeout(function () { window.open("/logged-out", "_self"); }, 500);
}
function rebootButton() {
    if (confirm("Confirm Restart?!")) {
        httpRequest("GET", "/reboot");
    }
}
function systemInfo() {
    httpRequest("GET", "/systeminfo", {
        onload: (xhr) => {
            if (xhr.status === 200) {
                try {
                    const data = JSON.parse(xhr.responseText);
                    _("firmwareVersion").innerHTML = data.VERSION;
                    _("freeSD").innerHTML = data.SD.free;
                    _("usedSD").innerHTML = data.SD.used;
                    _("totalSD").innerHTML = data.SD.total;
                } catch (error) {
                    console.error("JSON Parsing Error: ", error);
                }
            } else {
                console.error("Request Error: " + xhr.status);
            }
        }
    });
}
function listFilesButton(folders) {
    _("drop-area").style.display = 'block';
    _("actualFolder").value = folders;
    let previousFolder = folders.substring(0, folders.lastIndexOf('/'));
    if (previousFolder === "") { previousFolder = "/"; }
    httpRequest("GET", "/listfiles?folder=" + folders, {
        onload: (xhr) => {
            if (xhr.status === 200) {
                const responseText = xhr.responseText;
                const lines = responseText.split('\n');
                let tableContent = "<table><tr><th align='left'>Name</th><th style=\"text-align=center;\">Size</th><th></th></tr>\n";
                tableContent += "<tr><th align='left'><a onclick=\"listFilesButton('" + previousFolder + "')\" href='javascript:void(0);'>... </a></th><th align='left'></th><th></th></tr>\n";
                let folder = "";
                const foldersArray = [];
                const filesArray = [];
                lines.forEach((line) => {
                    if (line) {
                        const type = line.substring(0, 2);
                        const path = line.substring(3, line.lastIndexOf(':'));
                        const filename = line.substring(3, line.lastIndexOf(':'));
                        const size = line.substring(line.lastIndexOf(':') + 1);
                        if (type === "pa") {
                            if (path !== "" && folder !== "/") folder = path + (path.endsWith("/") ? "" : "/");
                        } else if (type === "Fo") {
                            foldersArray.push({ path: folder + path, name: filename });
                        } else if (type === "Fi") {
                            filesArray.push({ path: folder + path, name: filename, size });
                        }
                    }
                });
                foldersArray.sort((a, b) => a.name.localeCompare(b.name));
                filesArray.sort((a, b) => a.name.localeCompare(b.name));
                foldersArray.forEach((item) => {
                    tableContent += "<tr align='left'><td><a onclick=\"listFilesButton('" + item.path + "')\" href='javascript:void(0);'>" + item.name + "</a></td>";
                    tableContent += "<td></td>\n";
                    tableContent += "<td><i style=\"color: #e0d204;\" class=\"gg-folder\" onclick=\"listFilesButton('" + item.path + "')\"></i>&nbsp&nbsp";
                    tableContent += "<i style=\"color: #e0d204;\" class=\"gg-rename\" onclick=\"renameFile('" + item.path + "', '" + item.name + "')\"></i>&nbsp&nbsp";
                    tableContent += "<i style=\"color: #e0d204;\" class=\"gg-trash\" onclick=\"downloadDeleteButton('" + item.path + "', 'delete')\"></i></td></tr>\n\n";
                });
                filesArray.forEach((item) => {
                    tableContent += "<tr align='left'><td>" + item.name;
                    if (item.name.substring(item.name.lastIndexOf('.') + 1).toLowerCase() === "bin") {
                        tableContent += "&nbsp<i class=\"gg-arrow-top-right-r\" onclick=\"startUpdate('" + item.path + "')\"></i>";
                    }
                    tableContent += "</td>\n";
                    tableContent += "<td style=\"font-size: 10px; text-align=center;\">" + item.size + "</td>\n";
                    tableContent += "<td><i class=\"gg-arrow-down-r\" onclick=\"downloadDeleteButton('" + item.path + "', 'download')\"></i>&nbsp&nbsp\n";
                    tableContent += "<i class=\"gg-rename\" onclick=\"renameFile('" + item.path + "', '" + item.name + "')\"></i>&nbsp&nbsp\n";
                    tableContent += "<i class=\"gg-trash\" onclick=\"downloadDeleteButton('" + item.path + "', 'delete')\"></i></td></tr>\n\n";
                });
                tableContent += "</table>";
                _("details").innerHTML = tableContent;
            } else {
                console.error("Request Error: " + xhr.status);
            }
        },
        onerror: () => {
            console.error("Network error while fetching file list.");
        }
    });
    _("detailsheader").innerHTML = "<h3>Files</h3>";
    _("updetailsheader").innerHTML = "<h3>Folder Actions: " +
        "<input type='file' id='fa' multiple style='display:none'>" +
        "<input type='file' id='fol' webkitdirectory directory multiple style='display:none'>" +
        "<button onclick=\"_('fa').click()\">Send Files</button>" +
        "<button onclick=\"_('fol').click()\">Send Folders</button>" +
        "<button onclick=\"CreateFolder('" + folders + "')\">Create Folder</button></h3>";
    _("fa").onchange = e => handleFileForm(e.target.files, folders);
    _("fol").onchange = e => handleFileForm(e.target.files, folders);
    _("updetails").innerHTML = "";
    _("OTAdetails").style.display = 'none';
    _("analysisOutput").style.display = 'none';
    _("spiffsInfo").style.display = 'none';
    _("uploadApp").style.display = 'none';
    _("uploadSpiffs").style.display = 'none';
}
function renameFile(filePath, oldName) {
    const actualFolder = _("actualFolder").value;
    const fileName = prompt("Enter the new name: ", oldName);
    if (isNullOrEmpty(fileName)) {
        window.alert("Invalid Name");
    } else {
        const formdata5 = new FormData();
        formdata5.append("filePath", filePath);
        formdata5.append("fileName", fileName);
        const xhr = httpRequest("POST", "/rename", { async: false, body: formdata5 });
        _("status").innerHTML = xhr.responseText;
        listFilesButton(actualFolder);
    }
}
function downloadDeleteButton(filename, action) {
    const urltocall = "/file?name=" + filename + "&action=" + action;
    const actualFolder = _("actualFolder").value;
    const isDelete = action === "delete";
    if (isDelete || action === "create") {
        if (!isDelete || confirm("Do you really want to DELETE the file: " + filename + " ?\n\nThis action can't be undone!")) {
            const xhr = httpRequest("GET", urltocall, { async: false });
            _("status").innerHTML = xhr.responseText;
            listFilesButton(actualFolder);
        }
        return;
    }
    if (action === "download") {
        _("status").innerHTML = "";
        window.open(urltocall, "_blank");
    }
}
function CreateFolder(folders) {
    const folderName = prompt("Folder Name", "");
    if (isNullOrEmpty(folderName)) {
        window.alert("Invalid Folder Name");
    } else {
        downloadDeleteButton(_("actualFolder").value + "/" + folderName, 'create');
    }
}
const addHighlight = (event) => {
    event.preventDefault();
    event.currentTarget.classList.add("highlight");
};
const removeHighlight = (event) => {
    event.preventDefault();
    event.currentTarget.classList.remove("highlight");
};
window.addEventListener("load", () => {
    const dropArea = _("drop-area");
    dropArea.addEventListener("dragenter", addHighlight, false);
    dropArea.addEventListener("dragover", addHighlight, false);
    dropArea.addEventListener("dragleave", removeHighlight, false);
    dropArea.addEventListener("drop", drop, false);
});
let totalFiles = 0;
let completedFiles = 0;
function writeSendForm() {
    const uploadform =
        "<p>Send files</p>" +
        "<div id=\"file-progress-container\"></div>";
    _("updetails").innerHTML = uploadform;
}
async function drop(event) {
    event.preventDefault();
    _("drop-area").classList.remove("highlight");
    const items = event.dataTransfer.items;
    const filesQ = [];
    const promises = [];
    for (let i = 0; i < items.length; i++) {
        const entry = items[i].webkitGetAsEntry();
        if (entry) {
            promises.push(FileTree(entry, "", filesQ));
        }
    }
    await Promise.all(promises);
    handleFileForm(filesQ, _("actualFolder").value);
}
function FileTree(item, path = "", filesQ) {
    return new Promise((resolve) => {
        if (item.isFile) {
            item.file(function (file) {
                const fileWithPath = new File([file], path + file.name, { type: file.type });
                filesQ.push(fileWithPath);
                resolve();
            });
        } else if (item.isDirectory) {
            const dirReader = item.createReader();
            dirReader.readEntries((entries) => {
                const entryPromises = [];
                for (let i = 0; i < entries.length; i++) {
                    entryPromises.push(FileTree(entries[i], path + item.name + "/", filesQ));
                } Promise.all(entryPromises).then(resolve);
            });
        } else {
            resolve();
        }
    });
}
window.addEventListener("load", () => {
    listFilesButton("/");
    systemInfo();
});
let fileQueue = [];
let activeUploads = 0;
const maxConcurrentUploads = 3;
function handleFileForm(files, folder) {
    writeSendForm();
    fileQueue = Array.from(files);
    totalFiles = fileQueue.length;
    completedFiles = 0;
    activeUploads = 0;
    for (let i = 0; i < maxConcurrentUploads; i++) {
        processNextUpload(folder);
    }
}
function processNextUpload(folder) {
    if (fileQueue.length === 0) {
        if (activeUploads === 0) {
            _("status").innerHTML = "Upload Complete";
            const actualFolder = _("actualFolder").value;
            listFilesButton(actualFolder);
        }
        return;
    }
    if (activeUploads >= maxConcurrentUploads) return;
    const file = fileQueue.shift();
    activeUploads++;
    uploadFile(folder, file)
        .then(() => {
            activeUploads--;
            completedFiles++;
            _("status").innerHTML = `Uploaded ${completedFiles} of ${totalFiles} files.`;
            processNextUpload(folder);
        })
        .catch(() => {
            activeUploads--;
            _("status").innerHTML = "Upload Failed";
            processNextUpload(folder);
        });
}
function uploadFile(folder, file) {
    return new Promise((resolve, reject) => {
        const progressBarId = `${file.name}-progressBar`;
        if (!_(progressBarId)) {
            const fileProgressDiv = document.createElement("div");
            fileProgressDiv.innerHTML = `<p>${file.name}: <progress id="${progressBarId}" value="0" max="100" style="width:100%;"></progress></p>`;
            _("file-progress-container").appendChild(fileProgressDiv);
        }
        const formdata = new FormData();
        formdata.append("file", file, file.webkitRelativePath || file.name);
        formdata.append("folder", folder);
        const ajax = new XMLHttpRequest();
        ajax.upload.addEventListener("progress", (event) => {
            if (event.lengthComputable) {
                const percent = (event.loaded / event.total) * 100;
                _(progressBarId).value = Math.round(percent);
            }
        }, false);
        ajax.addEventListener("load", () => resolve(), false);
        ajax.addEventListener("error", () => reject(), false);
        ajax.addEventListener("abort", () => reject(), false);
        ajax.open("POST", "/");
        ajax.send(formdata);
    });
}
