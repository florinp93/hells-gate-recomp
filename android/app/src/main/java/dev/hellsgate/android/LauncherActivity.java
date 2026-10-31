package dev.hellsgate.android;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.util.concurrent.atomic.AtomicBoolean;

/** Imports the user's own extracted game folder and starts the game. */
public class LauncherActivity extends Activity {
    private static final int REQUEST_PICK_FOLDER = 1;
    private static final int REQUEST_IMPORT_SAVES = 2;
    private static final int REQUEST_EXPORT_SAVES = 3;

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancelled = new AtomicBoolean(false);

    private TextView status;
    private ProgressBar progressBar;
    private Button importButton;
    private Button playButton;
    private Button cancelButton;
    private Button importSavesButton;
    private Button exportSavesButton;
    private volatile boolean importing;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.BLACK);
        int pad = (int) (24 * getResources().getDisplayMetrics().density);
        root.setPadding(pad, pad, pad, pad);

        TextView title = new TextView(this);
        title.setText("Hell's Gate");
        title.setTextSize(28);
        title.setTextColor(Color.WHITE);
        title.setGravity(Gravity.CENTER);
        root.addView(title);

        TextView hint = new TextView(this);
        hint.setText("Bring your own copy of the game: extract your Xbox 360 disc image on a PC "
                + "(for example with extract-xiso), copy the extracted folder to this device, "
                + "then import the folder that contains default.xex.");
        hint.setTextColor(Color.LTGRAY);
        hint.setGravity(Gravity.CENTER);
        hint.setPadding(0, pad / 2, 0, pad / 2);
        root.addView(hint);

        status = new TextView(this);
        status.setTextColor(Color.WHITE);
        status.setGravity(Gravity.CENTER);
        root.addView(status);

        TextView logInfo = new TextView(this);
        logInfo.setTextColor(Color.GRAY);
        logInfo.setGravity(Gravity.CENTER);
        logInfo.setText("Log file: " + DiagLog.displayPath);
        root.addView(logInfo);

        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setVisibility(View.GONE);
        root.addView(progressBar, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        importButton = new Button(this);
        importButton.setText("Import game folder");
        importButton.setOnClickListener(v -> pickFolder());
        root.addView(importButton);

        playButton = new Button(this);
        playButton.setText("Play");
        playButton.setOnClickListener(v ->
                startActivity(new Intent(this, HellsGateActivity.class)));
        root.addView(playButton);

        android.widget.CheckBox fpsBox = new android.widget.CheckBox(this);
        fpsBox.setText("Show FPS counter in game");
        fpsBox.setTextColor(Color.WHITE);
        fpsBox.setChecked(GamePaths.prefs(this).getBoolean("show_fps", false));
        fpsBox.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("show_fps", on).apply());
        root.addView(fpsBox);

        android.widget.CheckBox fps30Box = new android.widget.CheckBox(this);
        fps30Box.setText("Lock to 30 fps (steadier and cooler; menu animations run slower)");
        fps30Box.setTextColor(Color.WHITE);
        fps30Box.setChecked(GamePaths.prefs(this).getBoolean("lock_30fps", false));
        fps30Box.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("lock_30fps", on).apply());
        root.addView(fps30Box);

        TextView aspectLabel = new TextView(this);
        aspectLabel.setText("Aspect ratio (wider than 16:9 widens the view to fill the screen)");
        aspectLabel.setTextColor(Color.WHITE);
        aspectLabel.setPadding(0, pad / 2, 0, 0);
        root.addView(aspectLabel);

        android.widget.RadioGroup aspectGroup = new android.widget.RadioGroup(this);
        aspectGroup.setOrientation(LinearLayout.HORIZONTAL);
        String currentAspect = GamePaths.prefs(this).getString("aspect", GamePaths.ASPECT_AUTO);
        for (String option : GamePaths.ASPECT_OPTIONS) {
            android.widget.RadioButton button = new android.widget.RadioButton(this);
            button.setText(option);
            button.setTextColor(Color.WHITE);
            button.setId(View.generateViewId());
            aspectGroup.addView(button);
            if (option.equals(currentAspect)) button.setChecked(true);
            button.setOnCheckedChangeListener((b, on) -> {
                if (on) GamePaths.prefs(this).edit().putString("aspect", option).apply();
            });
        }
        root.addView(aspectGroup);

        android.widget.CheckBox rpBox = new android.widget.CheckBox(this);
        rpBox.setText("Faster rendering (experimental: classic render passes; "
                + "may cause wrong colours or freezes)");
        rpBox.setTextColor(Color.WHITE);
        rpBox.setChecked(GamePaths.prefs(this).getBoolean("classic_render_passes", false));
        rpBox.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("classic_render_passes", on).apply());
        root.addView(rpBox);

        android.widget.CheckBox shadowBox = new android.widget.CheckBox(this);
        shadowBox.setText("Fast memory tracking (experimental: big speed-up, but currently causes "
                + "graphics glitches)");
        shadowBox.setTextColor(Color.WHITE);
        shadowBox.setChecked(GamePaths.prefs(this).getBoolean("protection_shadow", false));
        shadowBox.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("protection_shadow", on).apply());
        root.addView(shadowBox);

        android.widget.CheckBox profBox = new android.widget.CheckBox(this);
        profBox.setText("Diagnostics: profile slow threads (for bug reports only)");
        profBox.setTextColor(Color.GRAY);
        profBox.setChecked(GamePaths.prefs(this).getBoolean("profiler", false));
        profBox.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("profiler", on).apply());
        root.addView(profBox);

        android.widget.CheckBox gpuTimingBox = new android.widget.CheckBox(this);
        gpuTimingBox.setText("Diagnostics: log GPU time per render pass (for bug reports only)");
        gpuTimingBox.setTextColor(Color.GRAY);
        gpuTimingBox.setChecked(GamePaths.prefs(this).getBoolean("gpu_timing", false));
        gpuTimingBox.setOnCheckedChangeListener((b, on) ->
                GamePaths.prefs(this).edit().putBoolean("gpu_timing", on).apply());
        root.addView(gpuTimingBox);

        importSavesButton = new Button(this);
        importSavesButton.setText("Import saves");
        importSavesButton.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_IMPORT_SAVES));
        root.addView(importSavesButton);

        exportSavesButton = new Button(this);
        exportSavesButton.setText("Export saves (backup)");
        exportSavesButton.setOnClickListener(v -> startActivityForResult(
                new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST_EXPORT_SAVES));
        root.addView(exportSavesButton);

        cancelButton = new Button(this);
        cancelButton.setText("Cancel import");
        cancelButton.setVisibility(View.GONE);
        cancelButton.setOnClickListener(v -> cancelled.set(true));
        root.addView(cancelButton);

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        scroll.setBackgroundColor(Color.BLACK);
        scroll.setFillViewport(true);
        scroll.addView(root);
        setContentView(scroll);
        refresh();
    }

    private void refresh() {
        boolean ready = GamePaths.isGameImported(this);
        playButton.setEnabled(ready && !importing);
        importButton.setEnabled(!importing);
        importSavesButton.setEnabled(!importing);
        exportSavesButton.setEnabled(!importing);
        if (!importing) {
            status.setText(ready ? "Game imported. Ready to play."
                                 : "No game imported yet.");
        }
    }

    private void pickFolder() {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        startActivityForResult(intent, REQUEST_PICK_FOLDER);
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (resultCode != RESULT_OK || data == null || data.getData() == null) return;
        if (requestCode == REQUEST_PICK_FOLDER) {
            startImport(data.getData());
        } else if (requestCode == REQUEST_IMPORT_SAVES || requestCode == REQUEST_EXPORT_SAVES) {
            final boolean export = requestCode == REQUEST_EXPORT_SAVES;
            final Uri tree = data.getData();
            status.setText(export ? "Exporting saves..." : "Importing saves...");
            new Thread(() -> {
                String msg;
                try {
                    msg = export ? SaveTransfer.exportSaves(this, tree)
                                 : SaveTransfer.importSaves(this, tree);
                } catch (Exception e) {
                    msg = (export ? "Export failed: " : "Import failed: ")
                            + (e.getMessage() != null ? e.getMessage() : e.toString());
                }
                DiagLog.line("Saves: " + msg);
                final String m = msg;
                ui.post(() -> status.setText(m));
            }, "save-transfer").start();
        }
    }

    private void startImport(Uri treeUri) {
        importing = true;
        cancelled.set(false);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        progressBar.setVisibility(View.VISIBLE);
        progressBar.setProgress(0);
        cancelButton.setVisibility(View.VISIBLE);
        status.setText("Scanning folder...");
        refresh();

        final File destination = GamePaths.game(this);
        new Thread(() -> {
            String result;
            try {
                GameImporter.importTree(this, treeUri, destination,
                        (copied, total, file) -> ui.post(() -> {
                            if (total > 0) {
                                progressBar.setProgress((int) (copied * 1000 / total));
                            }
                            status.setText(String.format("Copying %s\n%d / %d MB", file,
                                    copied >> 20, total >> 20));
                        }),
                        cancelled);
                result = null;
            } catch (Exception e) {
                result = e.getMessage() != null ? e.getMessage() : e.toString();
            }
            final String error = result;
            ui.post(() -> {
                importing = false;
                getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
                progressBar.setVisibility(View.GONE);
                cancelButton.setVisibility(View.GONE);
                refresh();
                if (error != null) {
                    status.setText("Import failed: " + error);
                }
            });
        }, "game-import").start();
    }
}
