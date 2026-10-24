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

    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean cancelled = new AtomicBoolean(false);

    private TextView status;
    private ProgressBar progressBar;
    private Button importButton;
    private Button playButton;
    private Button cancelButton;
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

        cancelButton = new Button(this);
        cancelButton.setText("Cancel import");
        cancelButton.setVisibility(View.GONE);
        cancelButton.setOnClickListener(v -> cancelled.set(true));
        root.addView(cancelButton);

        setContentView(root);
        refresh();
    }

    private void refresh() {
        boolean ready = GamePaths.isGameImported(this);
        playButton.setEnabled(ready && !importing);
        importButton.setEnabled(!importing);
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
        if (requestCode == REQUEST_PICK_FOLDER && resultCode == RESULT_OK && data != null
                && data.getData() != null) {
            startImport(data.getData());
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
