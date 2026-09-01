'use strict';

// Probe what dlopen-related symbols are available
var syms = [
    "android_dlopen_ext",
    "dlopen",
    "__loader_dlopen",
    "__loader_android_dlopen_ext",
    "__dl__Z20android_dlopen_extPKciPK17android_dlextinfo",
    "__dl___loader_android_dlopen_ext"
];

var libs = [null, "libdl.so", "libdl_android.so", "linker64", "/apex/com.android.runtime/bin/linker64"];

console.log("=== Probing dlopen symbols ===");
for (var i = 0; i < libs.length; i++) {
    for (var j = 0; j < syms.length; j++) {
        try {
            var r = Module.findExportByName(libs[i], syms[j]);
            if (r !== null) {
                console.log("FOUND: " + (libs[i] || "null") + " :: " + syms[j] + " => " + r);
            }
        } catch(e) {
            console.log("ERROR: " + (libs[i] || "null") + " :: " + syms[j] + " => " + e.message);
        }
    }
}

// Also check what modules are loaded
console.log("\n=== Loaded modules (first 30) ===");
var mods = Process.enumerateModules();
for (var i = 0; i < Math.min(30, mods.length); i++) {
    console.log(mods[i].name + " @ " + mods[i].base + " size=" + mods[i].size);
}

// Try enumerating exports from libdl.so
console.log("\n=== libdl.so exports ===");
try {
    var libdl = Process.findModuleByName("libdl.so");
    if (libdl) {
        var exports = libdl.enumerateExports();
        for (var i = 0; i < exports.length; i++) {
            console.log("  " + exports[i].type + " " + exports[i].name + " => " + exports[i].address);
        }
    } else {
        console.log("  libdl.so not found as module");
    }
} catch(e) {
    console.log("  Error: " + e.message);
}

// Also check libdl_android.so
console.log("\n=== libdl_android.so exports ===");
try {
    var libdla = Process.findModuleByName("libdl_android.so");
    if (libdla) {
        var exports = libdla.enumerateExports();
        for (var i = 0; i < exports.length; i++) {
            console.log("  " + exports[i].type + " " + exports[i].name + " => " + exports[i].address);
        }
    } else {
        console.log("  libdl_android.so not found as module");
    }
} catch(e) {
    console.log("  Error: " + e.message);
}
