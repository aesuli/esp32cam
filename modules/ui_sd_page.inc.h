#pragma once

// SD browser page template.

static const char SD_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>__PAGE_TITLE__</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:Arial,sans-serif;background:#1a1a2e;color:#eee;min-height:100vh}
nav{background:#16213e;padding:8px 20px;display:flex;gap:12px;flex-wrap:wrap}
nav a{color:#eee;text-decoration:none;padding:8px 12px;border-radius:4px;border:1px solid #234573;cursor:pointer}
nav a:hover{background:#234573}
header{background:#16213e;padding:12px 20px;display:flex;align-items:center;justify-content:space-between}
header h1{color:#e94560;font-size:1.3em}
.container{max-width:1080px;margin:0 auto;padding:12px}
.actions{display:flex;gap:8px;margin-bottom:10px;flex-wrap:wrap}
.actions label,.actions button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.actions button:hover{background:#234573}
.actions button:disabled{opacity:.55;cursor:not-allowed}
.actions input[type=file]{display:none}
.folder-create{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:10px}
.folder-create input{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px;min-width:200px}
.folder-create button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.9em}
.folder-create button:hover{background:#234573}
.dropzone{border:2px dashed #234573;border-radius:8px;padding:14px 12px;margin-bottom:10px;text-align:center;color:#9ec5ff;background:#111c38;transition:background .15s,border-color .15s,color .15s}
.dropzone strong{color:#dbeafe}
.dropzone.active{border-color:#7dd3fc;background:#0d2f47;color:#dbeafe}
.pathbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.pathbar button{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 12px;border-radius:4px;cursor:pointer;font-size:.85em}
.pathbar button:hover{background:#234573}
.crumbs{font-size:.88em;color:#bbb;word-break:break-all}
.crumbs a{color:#7dd3fc;text-decoration:none}
.crumbs a:hover{text-decoration:underline}
.sortbar{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-bottom:10px}
.sortbar label{font-size:.85em;color:#bbb}
.sortbar select{background:#16213e;color:#fff;border:1px solid #234573;padding:8px 10px;border-radius:4px}
.status{min-height:20px;margin-bottom:12px;color:#7dd3fc;font-size:.9em}
.status.error{color:#ff8a8a}
.file-list{display:flex;flex-direction:column;gap:12px}
.group{background:#16213e;border:1px solid #234573;border-radius:8px;padding:10px}
.group h3{color:#7dd3fc;font-size:.95em;margin-bottom:8px}
.selection-summary{font-size:.82em;color:#bbb;margin:-2px 0 10px}
.file-item{display:flex;align-items:center;gap:8px;background:#0f3460;padding:10px;border-radius:6px;border:1px solid #234573;margin-bottom:8px}
.file-item.selected{border-color:#7dd3fc;box-shadow:0 0 0 1px rgba(125,211,252,.35)}
.item-select{display:flex;align-items:center;justify-content:center;flex:0 0 auto;margin-right:2px}
.item-select input{width:18px;height:18px;cursor:pointer}
.file-left{display:flex;align-items:center;gap:10px;min-width:0;flex:1 1 auto}
.thumb{width:72px;height:54px;border-radius:4px;object-fit:cover;background:#111;border:1px solid #234573;display:block}
.meta{display:flex;flex-direction:column;min-width:0}
.meta strong{color:#eee;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:360px}
.meta span{font-size:.82em;color:#bbb}
.file-actions{display:flex;gap:6px;flex-wrap:wrap;justify-content:flex-end}
.file-actions{margin-left:auto}
.file-actions a,.file-actions button{background:#e94560;color:#fff;border:none;border-radius:4px;padding:6px 10px;cursor:pointer;text-decoration:none;font-size:.8em}
.file-actions button:hover,.file-actions a:hover{background:#c73652}
.empty{text-align:center;padding:40px;color:#bbb}
@media (max-width:700px){.file-item{flex-direction:column;align-items:flex-start}.meta strong{max-width:100%}.thumb{width:100px;height:75px}.file-actions{justify-content:flex-start}}
</style>
</head>
<body>
__APP_NAV__
<header>
  <h1>💾 SD Card Browser</h1>
</header>
<div class="container">
  <div class="actions">
    <button onclick="loadFiles()">Refresh</button>
    <button id="select_all" onclick="toggleSelectAllFiles()" disabled>Select All</button>
    <button id="download_selected" onclick="downloadSelectedFiles()" disabled>Download Selected</button>
    <button id="delete_selected" onclick="deleteSelectedFiles()" disabled>Delete Selected</button>
    <label>Upload: <input id="upload_file" type="file" onchange="uploadFile(this)"></label>
  </div>
  <div class="folder-create">
    <input id="new_folder_name" type="text" placeholder="New folder name" maxlength="64">
    <button onclick="createFolder()">Create Folder</button>
  </div>
  <div id="dropzone" class="dropzone"><strong>Drag and drop files here</strong> to upload into the current folder</div>
  <div class="pathbar">
    <button onclick="goUp()">Up</button>
    <div id="crumbs" class="crumbs"></div>
  </div>
  <div class="sortbar">
    <label for="sort_by">Sort by</label>
    <select id="sort_by" onchange="onSortChanged()">
      <option value="name">Name</option>
      <option value="size">Size</option>
      <option value="type">Type</option>
    </select>
    <label for="sort_dir">Direction</label>
    <select id="sort_dir" onchange="onSortChanged()">
      <option value="asc">Ascending</option>
      <option value="desc">Descending</option>
    </select>
  </div>
  <div id="status" class="status"></div>
  <div id="file_list" class="file-list"></div>
</div>
__APP_FOOTER__
<script>
var allItems=[];
var currentDir='/';
var transferBase='http://'+window.location.hostname+':82';
var transferToken=encodeURIComponent('__ROUTE_TOKEN__');
var selectedFiles={};
var visibleFileOrder=[];
var lastSelectionAnchor='';
function esc(s){return String(s).replace(/[&<>\"']/g,function(ch){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;','\'':'&#39;'}[ch];});}
function setStatus(msg,isError){
  var el=document.getElementById('status');
  el.textContent=msg||'';
  el.className=isError?'status error':'status';
}
function normalizePath(p){
  var path=String(p||'/').replace(/\\/g,'/');
  if(!path.startsWith('/')) path='/'+path;
  if(path.length>1 && path.endsWith('/')) path=path.slice(0,-1);
  return path;
}
function validFolderName(name){
  var n=String(name||'').trim();
  if(!n||n==='.'||n==='..')return false;
  if(n.indexOf('/')!==-1||n.indexOf('\\')!==-1||n.indexOf('..')!==-1)return false;
  return true;
}
function baseName(path){
  var clean=normalizePath(path);
  if(clean==='/') return '/';
  var idx=clean.lastIndexOf('/');
  return idx>=0?clean.slice(idx+1):clean;
}
function isImage(path){
  var p=String(path).toLowerCase();
  return p.endsWith('.jpg')||p.endsWith('.jpeg')||p.endsWith('.png')||p.endsWith('.gif')||p.endsWith('.webp')||p.endsWith('.bmp');
}
function formatSize(bytes){
  var n=Number(bytes)||0;
  if(n<1024)return n+' B';
  if(n<1024*1024)return (n/1024).toFixed(1)+' KB';
  return (n/(1024*1024)).toFixed(2)+' MB';
}
function quotedName(path){
  return '"'+baseName(path)+'"';
}
function getSelectedFiles(){
  return Object.keys(selectedFiles).filter(function(path){return !!selectedFiles[path];});
}
function getVisibleFilePaths(){
  return visibleFileOrder.slice();
}
function syncSelectedFiles(){
  var valid={};
  allItems.forEach(function(item){
    if(!item.isDir && selectedFiles[item.path]){
      valid[item.path]=true;
    }
  });
  selectedFiles=valid;
}
function updateBulkDeleteButton(){
  var count=getSelectedFiles().length;
  var visiblePaths=getVisibleFilePaths();
  var visibleCount=visiblePaths.length;
  var selectedVisibleCount=visiblePaths.filter(function(path){return !!selectedFiles[path];}).length;
  var allVisibleSelected=visibleCount>0&&selectedVisibleCount===visibleCount;
  var deleteButton=document.getElementById('delete_selected');
  var downloadButton=document.getElementById('download_selected');
  var selectAllButton=document.getElementById('select_all');
  var hasSelection=count>0;
  deleteButton.disabled=!hasSelection;
  downloadButton.disabled=!hasSelection;
  selectAllButton.disabled=visibleCount===0;
  selectAllButton.textContent=allVisibleSelected?'Clear All':'Select All';
  deleteButton.textContent=hasSelection?'Delete Selected ('+count+')':'Delete Selected';
  downloadButton.textContent=hasSelection?'Download Selected ('+count+')':'Download Selected';
}
function toggleFileSelection(path,checked){
  var normalized=String(path||'');
  if(!normalized)return;
  if(checked) selectedFiles[normalized]=true;
  else delete selectedFiles[normalized];
  lastSelectionAnchor=normalized;
  applySortAndRender();
}
function applyRangeSelection(anchorPath,targetPath,checked){
  var paths=getVisibleFilePaths();
  var start=paths.indexOf(anchorPath);
  var end=paths.indexOf(targetPath);
  if(start<0||end<0){
    toggleFileSelection(targetPath,checked);
    return;
  }
  var low=Math.min(start,end);
  var high=Math.max(start,end);
  for(var i=low;i<=high;i++){
    if(checked) selectedFiles[paths[i]]=true;
    else delete selectedFiles[paths[i]];
  }
  lastSelectionAnchor=targetPath;
  applySortAndRender();
}
function onFileCheckboxClick(path,input,event){
  var checked=!!(input&&input.checked);
  var useRange=!!(event&&event.shiftKey&&lastSelectionAnchor);
  if(useRange){
    applyRangeSelection(lastSelectionAnchor,path,checked);
    return;
  }
  toggleFileSelection(path,checked);
}
function toggleSelectAllFiles(){
  var paths=getVisibleFilePaths();
  if(!paths.length){
    setStatus('No files to select in this folder.',true);
    return;
  }
  var allSelected=paths.every(function(path){return !!selectedFiles[path];});
  if(allSelected){
    paths.forEach(function(path){delete selectedFiles[path];});
    setStatus('Selection cleared.',false);
  }else{
    paths.forEach(function(path){selectedFiles[path]=true;});
    setStatus(paths.length+' file(s) selected.',false);
  }
  lastSelectionAnchor='';
  applySortAndRender();
}
function buildDeleteBody(paths){
  return paths.map(function(path){return 'file='+encodeURIComponent(path);}).join('&');
}
function deleteFiles(paths){
  var items=(paths||[]).filter(function(path){return !!path;});
  if(!items.length){
    setStatus('Select at least one file to delete.',true);
    return Promise.resolve(false);
  }
  var label=items.length===1?quotedName(items[0]):String(items.length)+' files';
  setStatus('Deleting '+label+'...',false);
  return fetch('/sd/delete',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:buildDeleteBody(items)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Delete failed');}
      items.forEach(function(path){delete selectedFiles[path];});
      setStatus(t||'File deleted',false);
      loadFiles();
      return true;
    });
  }).catch(function(e){
    setStatus(e.message||'Delete failed',true);
    return false;
  });
}
function downloadFile(path){
  var anchor=document.createElement('a');
  anchor.href=transferBase+'/sd/download?file='+encodeURIComponent(path)+'&t='+transferToken;
  anchor.download=baseName(path);
  anchor.style.display='none';
  document.body.appendChild(anchor);
  anchor.click();
  document.body.removeChild(anchor);
}
function downloadSelectedFiles(){
  var files=getSelectedFiles();
  if(!files.length){
    setStatus('Select at least one file to download.',true);
    return;
  }
  for(var i=0;i<files.length;i++){
    (function(path,delayMs){
      setTimeout(function(){downloadFile(path);},delayMs);
    })(files[i],i*120);
  }
  setStatus('Started '+files.length+' download(s). Your browser may ask for permission.',false);
}
function renderCrumbs(){
  var el=document.getElementById('crumbs');
  var clean=normalizePath(currentDir);
  if(clean==='/'){
    el.innerHTML='<strong>/</strong>';
    return;
  }
  var parts=clean.slice(1).split('/');
  var acc='';
  var links=['<a href="#" onclick="openDir(\'/\');return false;">/</a>'];
  for(var i=0;i<parts.length;i++){
    acc+='/'+parts[i];
    if(i===parts.length-1){
      links.push('<strong>'+esc(parts[i])+'</strong>');
    }else{
      links.push('<a href="#" onclick="openDir(\''+acc.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">'+esc(parts[i])+'</a>');
    }
  }
  el.innerHTML=links.join(' / ');
}
function sortItems(items){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value==='desc'?-1:1;
  function splitNumericPrefix(name){
    var text=String(name||'');
    var m=text.match(/^(\d+)(.*)$/);
    if(!m) return {hasPrefix:false,num:0,rest:text};
    return {hasPrefix:true,num:parseInt(m[1],10)||0,rest:m[2]};
  }
  function compareSmartName(aName,bName){
    var aParts=splitNumericPrefix(aName);
    var bParts=splitNumericPrefix(bName);
    if(aParts.hasPrefix&&bParts.hasPrefix){
      if(aParts.num!==bParts.num) return aParts.num-bParts.num;
      return aParts.rest.localeCompare(bParts.rest);
    }
    if(aParts.hasPrefix!==bParts.hasPrefix) return aParts.hasPrefix?-1:1;
    return String(aName||'').localeCompare(String(bName||''));
  }
  return items.slice().sort(function(a,b){
    if(a.isDir!==b.isDir) return a.isDir?-1:1;
    var ba=String(a.name||'');
    var bb=String(b.name||'');
    var ta=ba.indexOf('.')>=0?ba.slice(ba.lastIndexOf('.')+1).toLowerCase():'';
    var tb=bb.indexOf('.')>=0?bb.slice(bb.lastIndexOf('.')+1).toLowerCase():'';

    var cmp=0;
    if(by==='size') cmp=(Number(a.size)||0)-(Number(b.size)||0);
    else if(by==='type') cmp=ta.localeCompare(tb)||compareSmartName(ba,bb);
    else cmp=compareSmartName(ba,bb);

    return cmp*dir;
  });
}
function renderItems(items){
  var list=document.getElementById('file_list');
  syncSelectedFiles();
  visibleFileOrder=items.filter(function(item){return !item.isDir;}).map(function(item){return item.path;});
  if(lastSelectionAnchor&&visibleFileOrder.indexOf(lastSelectionAnchor)<0){
    lastSelectionAnchor='';
  }
  updateBulkDeleteButton();
  if(!items.length){
    list.innerHTML='<div class="empty">This folder is empty</div>';
    setStatus('Folder '+currentDir+' is empty.',false);
    return;
  }

  var totalSize=0;
  items.forEach(function(f){if(!f.isDir)totalSize+=Number(f.size)||0;});

  var rows=items.map(function(item){
    var isSelected=!!selectedFiles[item.path];
    var selectionClass=isSelected?' selected':'';
    var selectionCell=item.isDir
      ?'<div class="item-select"></div>'
      :'<label class="item-select" aria-label="Select '+esc(item.name)+'"><input type="checkbox" '+(isSelected?'checked ':'')+'onclick="onFileCheckboxClick(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\',this,event)"></label>';
    if(item.isDir){
      return '<div class="file-item'+selectionClass+'">'
        +selectionCell
        +'<div class="file-left" ondblclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')"><div class="thumb"></div><div class="meta"><strong>'+esc(item.name)+'</strong><span>Folder • '+esc(item.path)+'</span></div></div>'
        +'<div class="file-actions"><button onclick="openDir(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Open</button><button onclick="deleteFolder(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
        +'</div>';
    }

    var preview=isImage(item.path)
      ?'<img class="thumb" loading="lazy" src="'+transferBase+'/sd/view?file='+encodeURIComponent(item.path)+'&t='+transferToken+'" alt="preview">'
      :'<div class="thumb"></div>';
    return '<div class="file-item'+selectionClass+'">'
      +selectionCell
      +'<div class="file-left" ondblclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">'+preview+'<div class="meta"><strong>'+esc(item.name)+'</strong><span>'+formatSize(item.size)+' • '+esc(item.path)+'</span></div></div>'
      +'<div class="file-actions"><a href="'+transferBase+'/sd/download?file='+encodeURIComponent(item.path)+'&t='+transferToken+'">Download</a><a href="#" onclick="openFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\');return false;">Open</a><button onclick="deleteFile(\''+item.path.replace(/\\/g,'\\\\').replace(/'/g,"\\'")+'\')">Delete</button></div>'
      +'</div>';
  }).join('');

  var selectedCount=getSelectedFiles().length;
  var summary=selectedCount>0?'<div class="selection-summary">'+selectedCount+' file(s) selected</div>':'';
  list.innerHTML='<section class="group"><h3>'+esc(currentDir)+'</h3>'+summary+rows+'</section>';
  var fileCount=items.filter(function(i){return !i.isDir;}).length;
  var dirCount=items.filter(function(i){return i.isDir;}).length;
  setStatus(dirCount+' folder(s), '+fileCount+' file(s), '+formatSize(totalSize),false);
}
function applySortAndRender(){
  renderCrumbs();
  renderItems(sortItems(allItems));
}
function openDir(path){
  currentDir=normalizePath(path);
  loadFiles();
}
function openFile(path){
  var p=String(path||'').toLowerCase();
  var isVideo=p.endsWith('.avi')||p.endsWith('.mp4')||p.endsWith('.mjpg')||p.endsWith('.mov')||p.endsWith('.webm');
  if(isVideo){
    window.location.assign('/sd/player?file='+encodeURIComponent(path));
    return;
  }
  window.location.assign(transferBase+'/sd/view?file='+encodeURIComponent(path)+'&t='+transferToken);
}
function goUp(){
  if(currentDir==='/') return;
  var parent=currentDir.substring(0,currentDir.lastIndexOf('/'));
  if(!parent) parent='/';
  openDir(parent);
}
function loadFiles(){
  setStatus('Loading files...',false);
  fetch('/sd/list?dir='+encodeURIComponent(currentDir)).then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load files');}
      return d;
    });
  }).then(function(d){
    currentDir=normalizePath(d.dir||currentDir);
    allItems=d.items||[];
    syncSelectedFiles();
    applySortAndRender();
  }).catch(function(e){
    document.getElementById('file_list').innerHTML='<div class="empty">Error loading files</div>';
    visibleFileOrder=[];
    lastSelectionAnchor='';
    updateBulkDeleteButton();
    setStatus(e.message||'Failed to load files',true);
  });
}
function deleteFile(name){
  if(!confirm('Delete '+quotedName(name)+'?'))return;
  deleteFiles([name]);
}
function deleteSelectedFiles(){
  var files=getSelectedFiles();
  if(!files.length){
    setStatus('Select at least one file to delete.',true);
    return;
  }
  if(!confirm('Delete '+files.length+' selected file(s)?'))return;
  deleteFiles(files);
}
function createFolder(){
  var input=document.getElementById('new_folder_name');
  var name=String(input.value||'').trim();
  if(!validFolderName(name)){
    setStatus('Invalid folder name',true);
    return;
  }
  setStatus('Creating folder '+name+'...',false);
  var body='dir='+encodeURIComponent(currentDir)+'&name='+encodeURIComponent(name);
  fetch('/sd/mkdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to create folder');}
      setStatus(t||'Folder created',false);
      input.value='';
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to create folder',true);});
}
function deleteFolder(path){
  if(path==='/'||!path){
    setStatus('Cannot delete root folder',true);
    return;
  }
  if(!confirm('Delete folder '+path+' and all contents?'))return;
  setStatus('Deleting folder '+path+'...',false);
  fetch('/sd/rmdir',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'dir='+encodeURIComponent(path)}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Failed to delete folder');}
      setStatus(t||'Folder deleted',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Failed to delete folder',true);});
}
function uploadFileObject(file){
  if(!file)return;
  setStatus('Uploading '+file.name+'...',false);
  var fd=new FormData();fd.append('file',file);
  fetch(transferBase+'/sd/upload?t='+transferToken+'&dir='+encodeURIComponent(currentDir),{method:'POST',body:fd}).then(function(r){
    return r.text().then(function(t){
      if(!r.ok){throw new Error(t||'Upload failed');}
      setStatus(t||'Upload complete',false);
      loadFiles();
    });
  }).catch(function(e){setStatus(e.message||'Upload failed',true);});
}
function uploadFile(input){
  if(!input.files.length)return;
  uploadFileObject(input.files[0]);
  input.value='';
}
function isValidSortBy(v){
  return v==='name'||v==='size'||v==='type';
}
function isValidSortDir(v){
  return v==='asc'||v==='desc';
}
function saveSortPreference(){
  var by=document.getElementById('sort_by').value;
  var dir=document.getElementById('sort_dir').value;
  fetch('/sd/sort',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'by='+encodeURIComponent(by)+'&dir='+encodeURIComponent(dir)}).catch(function(){});
}
function loadSortPreference(){
  return fetch('/sd/sort').then(function(r){
    return r.json().then(function(d){
      if(!r.ok){throw new Error(d.error||'Failed to load sort preference');}
      return d;
    });
  }).then(function(d){
    document.getElementById('sort_by').value=isValidSortBy(d.by)?d.by:'name';
    document.getElementById('sort_dir').value=isValidSortDir(d.dir)?d.dir:'asc';
  }).catch(function(){
    document.getElementById('sort_by').value='name';
    document.getElementById('sort_dir').value='asc';
  });
}
function onSortChanged(){
  applySortAndRender();
  saveSortPreference();
}
function setupDropzone(){
  var zone=document.getElementById('dropzone');
  if(!zone)return;
  function over(e){
    e.preventDefault();
    zone.classList.add('active');
  }
  function leave(e){
    e.preventDefault();
    zone.classList.remove('active');
  }
  zone.addEventListener('dragenter',over);
  zone.addEventListener('dragover',over);
  zone.addEventListener('dragleave',leave);
  zone.addEventListener('drop',function(e){
    e.preventDefault();
    zone.classList.remove('active');
    var files=e.dataTransfer&&e.dataTransfer.files;
    if(!files||!files.length){
      setStatus('No file dropped',true);
      return;
    }
    uploadFileObject(files[0]);
  });
}
setupDropzone();
loadSortPreference().then(function(){loadFiles();});
</script>
</body>
</html>)html";

// ─── Motion pages (stored in flash) ───────────────────────────────────────────
