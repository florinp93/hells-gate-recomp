package dev.hellsgate.android;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.net.Uri;
import android.os.Build;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.system.Os;

import java.io.BufferedReader;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.List;
import java.util.Locale;

/**
 * Writes a live diagnostic log to Download/HellsGate/ (visible in the Files app): device info,
 * why the previous run ended, this app's logcat, native stdout/stderr, Java and native crashes.
 */
final class DiagLog {
    private static FileOutputStream out;
    private static ParcelFileDescriptor pfd;
    static String displayPath = "(log unavailable)";

    private DiagLog() {}

    static synchronized void init(Context ctx) {
        if (out != null) return;
        try {
            String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
            String name = "hellsgate-log-" + stamp + ".txt";
            ContentResolver cr = ctx.getContentResolver();
            ContentValues v = new ContentValues();
            v.put(MediaStore.MediaColumns.DISPLAY_NAME, name);
            v.put(MediaStore.MediaColumns.MIME_TYPE, "text/plain");
            v.put(MediaStore.MediaColumns.RELATIVE_PATH, "Download/HellsGate");
            Uri uri = cr.insert(MediaStore.Downloads.EXTERNAL_CONTENT_URI, v);
            if (uri == null) throw new IllegalStateException("MediaStore insert failed");
            pfd = cr.openFileDescriptor(uri, "w");
            out = new FileOutputStream(pfd.getFileDescriptor());
            displayPath = "Download/HellsGate/" + name;
        } catch (Throwable t) {
            displayPath = "(log unavailable: " + t + ")";
            return;
        }

        line("Hell's Gate Android log " + displayPath);
        line("Device: " + Build.MANUFACTURER + " " + Build.MODEL + " (" + Build.DEVICE + ", "
                + Build.HARDWARE + ", SoC " + (Build.VERSION.SDK_INT >= 31 ? Build.SOC_MODEL : "?")
                + ")");
        line("Android " + Build.VERSION.RELEASE + " (API " + Build.VERSION.SDK_INT + "), ABIs "
                + java.util.Arrays.toString(Build.SUPPORTED_ABIS));
        line("Page size: " + android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE));
        ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
        ((ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE)).getMemoryInfo(mi);
        line("RAM total " + (mi.totalMem >> 20) + " MB, avail " + (mi.availMem >> 20) + " MB");
        previousExits(ctx);

        // Native stdout/stderr (spdlog, fprintf, abort messages) go to the same file.
        try {
            FileDescriptor fd = pfd.getFileDescriptor();
            Os.dup2(fd, 1);
            Os.dup2(fd, 2);
        } catch (Throwable t) {
            line("dup2 failed: " + t);
        }

        try {
            System.loadLibrary("crashlog");
            installNative();
            nativeReady = true;
            line("Native crash handler installed");
        } catch (Throwable t) {
            line("Native crash handler unavailable: " + t);
        }

        final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, e) -> {
            StringWriter sw = new StringWriter();
            e.printStackTrace(new PrintWriter(sw));
            line("=== JAVA CRASH in " + thread.getName() + " ===\n" + sw);
            if (prev != null) prev.uncaughtException(thread, e);
        });

        Thread t = new Thread(DiagLog::pumpLogcat, "diag-logcat");
        t.setDaemon(true);
        t.start();

        final android.os.PowerManager pm =
                (android.os.PowerManager) ctx.getSystemService(Context.POWER_SERVICE);
        Thread perf = new Thread(() -> perfLoop(pm), "diag-perf");
        perf.setDaemon(true);
        perf.start();

        // The SDK's own log file (app storage) is copied into this log as it grows.
        final java.io.File sdkLog = new java.io.File(GamePaths.root(ctx), "logs/dantes_inferno.log");
        Thread tail = new Thread(() -> tailFile(sdkLog), "diag-sdklog");
        tail.setDaemon(true);
        tail.start();
    }

    private static void previousExits(Context ctx) {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = (ActivityManager) ctx.getSystemService(Context.ACTIVITY_SERVICE);
            List<ApplicationExitInfo> list = am.getHistoricalProcessExitReasons(
                    ctx.getPackageName(), 0, 5);
            line("--- previous process exits (newest first) ---");
            for (ApplicationExitInfo i : list) {
                line(new Date(i.getTimestamp()) + " reason=" + i.getReason() + " status="
                        + i.getStatus() + " importance=" + i.getImportance() + " pss="
                        + (i.getPss() >> 10) + "MB rss=" + (i.getRss() >> 10) + "MB desc="
                        + i.getDescription() + " (" + reasonName(i.getReason()) + ")");
            }
            line("--- end previous exits ---");
        } catch (Throwable t) {
            line("exit info unavailable: " + t);
        }
    }

    private static String reasonName(int r) {
        switch (r) {
            case ApplicationExitInfo.REASON_CRASH: return "JAVA_CRASH";
            case ApplicationExitInfo.REASON_CRASH_NATIVE: return "NATIVE_CRASH";
            case ApplicationExitInfo.REASON_ANR: return "ANR";
            case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY_KILL";
            case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
            case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
            case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
            case ApplicationExitInfo.REASON_INITIALIZATION_FAILURE: return "INIT_FAILURE";
            case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCES";
            default: return "OTHER";
        }
    }

    private static void pumpLogcat() {
        try {
            Process p = new ProcessBuilder("logcat", "-v", "threadtime").redirectErrorStream(true)
                    .start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            String s;
            while ((s = r.readLine()) != null) line(s);
        } catch (Throwable t) {
            line("logcat pump stopped: " + t);
        }
    }

    /** Every 5 s: per-thread CPU use of this process (top threads) and the thermal state. */
    private static void perfLoop(android.os.PowerManager pm) {
        final long hz = android.system.Os.sysconf(android.system.OsConstants._SC_CLK_TCK);
        java.util.Map<String, Long> prev = new java.util.HashMap<>();
        long prevWall = System.nanoTime();
        while (true) {
            try {
                Thread.sleep(5000);
            } catch (InterruptedException e) {
                return;
            }
            java.util.Map<String, Long> cur = new java.util.HashMap<>();
            java.util.Map<String, Long> curSys = new java.util.HashMap<>();
            java.util.Map<String, Long> curFlt = new java.util.HashMap<>();
            java.util.Map<String, String> states = new java.util.HashMap<>();
            java.util.Map<String, String> names = new java.util.HashMap<>();
            java.io.File[] tasks = new java.io.File("/proc/self/task").listFiles();
            if (tasks == null) continue;
            for (java.io.File t : tasks) {
                try {
                    String stat = new String(java.nio.file.Files.readAllBytes(
                            new java.io.File(t, "stat").toPath()), StandardCharsets.US_ASCII);
                    int l = stat.indexOf('('), r = stat.lastIndexOf(')');
                    String name = stat.substring(l + 1, r);
                    String[] f = stat.substring(r + 2).split(" ");
                    long ticks = Long.parseLong(f[11]) + Long.parseLong(f[12]);  // utime+stime
                    cur.put(t.getName(), ticks);
                    names.put(t.getName(), name);
                    curSys.put(t.getName(), Long.parseLong(f[12]));
                    curFlt.put(t.getName(), Long.parseLong(f[7]));  // minflt
                    states.put(t.getName(), f[0]);
                } catch (Throwable ignored) {
                }
            }
            long wall = System.nanoTime();
            double secs = (wall - prevWall) / 1e9;
            prevWall = wall;
            java.util.List<Object[]> rows = new java.util.ArrayList<>();
            double total = 0;
            for (java.util.Map.Entry<String, Long> e : cur.entrySet()) {
                Long before = prev.get(e.getKey());
                if (before == null) continue;
                double pct = (e.getValue() - before) * 100.0 / hz / secs;
                total += pct;
                if (pct >= 3) rows.add(new Object[] {pct, names.get(e.getKey()), e.getKey()});
            }
            // For pegged threads: user vs kernel split, state, current syscall, kernel wait point.
            StringBuilder peg = new StringBuilder();
            for (Object[] r : rows) {
                if ((double) r[0] < 90) continue;
                String tid = (String) r[2];
                Long s0 = prevSys.get(tid), s1 = curSys.get(tid);
                double sysPct = (s0 != null && s1 != null) ? (s1 - s0) * 100.0 / hz / secs : -1;
                Long f0 = prevFlt.get(tid), f1 = curFlt.get(tid);
                long faultsPerSec = (f0 != null && f1 != null) ? (long) ((f1 - f0) / secs) : -1;
                peg.append(String.format(Locale.US,
                        " [%s/%s state=%s kernel=%.0f%% minflt/s=%d syscall=%s wchan=%s %s]",
                        r[1], tid, states.get(tid), sysPct, faultsPerSec,
                        readSmall("/proc/self/task/" + tid + "/syscall"),
                        readSmall("/proc/self/task/" + tid + "/wchan"),
                        statusFields("/proc/self/task/" + tid + "/status")));
            }
            prevSys = curSys;
            prevFlt = curFlt;
            prev = cur;
            if (peg.length() > 0) line("PERF: pegged threads:" + peg);
            if (rows.isEmpty() && total < 5) continue;
            rows.sort((a, b) -> Double.compare((double) b[0], (double) a[0]));
            StringBuilder sb = new StringBuilder();
            sb.append(String.format(Locale.US, "PERF: process CPU %.0f%% (100%% = one core)", total));
            if (Build.VERSION.SDK_INT >= 29 && pm != null) {
                sb.append(", thermal ").append(pm.getCurrentThermalStatus());
            }
            sb.append(" | top threads:");
            for (int i = 0; i < Math.min(8, rows.size()); i++) {
                Object[] r = rows.get(i);
                sb.append(String.format(Locale.US, " %s/%s %.0f%%", r[1], r[2], (double) r[0]));
            }
            line(sb.toString());

            // Sample the hottest thread while it is pegged: up to 4 profiles, 30 s apart.
            Object[] top = rows.isEmpty() ? null : rows.get(0);
            // Rotate: hottest thread, GPU command processor, audio worker.
            String want = profilesTaken % 3 == 1 ? "GPU Commands"
                    : profilesTaken % 3 == 2 ? "Audio Worker" : null;
            if (top != null && want != null) {
                for (Object[] r : rows) {
                    if (((String) r[1]).startsWith(want)) { top = r; break; }
                }
            }
            boolean gpuTurn = want != null;
            // A thread pegged near 100% (possible hang): always allowed, every 20 s.
            boolean pegged = top != null && (double) top[0] >= 95;
            peggedTicks = pegged ? peggedTicks + 1 : 0;
            boolean hang = peggedTicks >= 2 && profilesTaken < 12
                    && System.nanoTime() - lastProfile > 20_000_000_000L;
            if (top != null && (hang || (((double) top[0] >= 70 || gpuTurn) && profilesTaken < 6
                    && System.nanoTime() - lastProfile > 30_000_000_000L)) && nativeReady) {
                profilesTaken++;
                lastProfile = System.nanoTime();
                line("PERF: profiling thread " + top[1] + "/" + top[2] + " for 5 s");
                try {
                    profileThread(Integer.parseInt((String) top[2]), 5000);
                } catch (Throwable t) {
                    line("profiler unavailable: " + t);
                }
            }
        }
    }

    private static java.util.Map<String, Long> prevSys = new java.util.HashMap<>();
    private static java.util.Map<String, Long> prevFlt = new java.util.HashMap<>();

    /** SigPnd/ShdPnd/SigBlk and context switch counts from a task's status file. */
    private static String statusFields(String path) {
        StringBuilder sb = new StringBuilder();
        try {
            for (String l : java.nio.file.Files.readAllLines(java.nio.file.Paths.get(path))) {
                if (l.startsWith("SigPnd") || l.startsWith("ShdPnd") || l.startsWith("SigBlk")
                        || l.startsWith("voluntary_ctxt") || l.startsWith("nonvoluntary_ctxt")) {
                    sb.append(l.replace("\t", "").replace("_ctxt_switches", "Cs")).append(' ');
                }
            }
        } catch (Throwable t) {
            return "status n/a";
        }
        return sb.toString().trim();
    }

    private static String readSmall(String path) {
        try {
            String v = new String(java.nio.file.Files.readAllBytes(java.nio.file.Paths.get(path)),
                    StandardCharsets.US_ASCII).trim();
            return v.length() > 120 ? v.substring(0, 120) : v;
        } catch (Throwable t) {
            return "n/a";
        }
    }

    private static int profilesTaken;
    private static int peggedTicks;
    private static long lastProfile;
    private static volatile boolean nativeReady;

    private static native void profileThread(int tid, int ms);

    private static void tailFile(java.io.File f) {
        long offset = f.exists() ? f.length() : 0;
        // The previous run's SDK log is complete on disk: copy its end first.
        if (offset > 0) {
            try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
                long start = Math.max(0, offset - 256 * 1024);
                r.seek(start);
                byte[] prev = new byte[(int) (offset - start)];
                r.readFully(prev);
                line("--- previous run SDK log (last " + prev.length + " bytes of " + f + ") ---");
                synchronized (DiagLog.class) {
                    if (out != null) out.write(prev);
                }
                line("--- end previous run SDK log; live SDK log follows ---");
            } catch (Throwable t) {
                line("previous SDK log unreadable: " + t);
            }
        }
        byte[] buf = new byte[16384];
        while (true) {
            try {
                if (f.exists()) {
                    if (f.length() < offset) offset = 0;  // truncated or rotated
                    try (java.io.RandomAccessFile r = new java.io.RandomAccessFile(f, "r")) {
                        r.seek(offset);
                        int n;
                        while ((n = r.read(buf)) > 0) {
                            synchronized (DiagLog.class) {
                                if (out != null) out.write(buf, 0, n);
                            }
                            offset += n;
                        }
                    }
                }
                Thread.sleep(50);
            } catch (InterruptedException e) {
                return;
            } catch (Throwable ignored) {
                try { Thread.sleep(500); } catch (InterruptedException e) { return; }
            }
        }
    }

    static synchronized void line(String s) {
        if (out == null) return;
        try {
            out.write((s + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (Throwable ignored) {
        }
    }

    private static native void installNative();
}
