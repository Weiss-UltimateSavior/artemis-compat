package com.ies_net.artemis.debug;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.DialogInterface;
import android.text.InputFilter;
import android.text.InputType;
import android.widget.EditText;

/**
 * Host-side input box for the engine's native [dialog] tag (protagonist name
 * entry, etc.). Drop this file next to {@code DebugBridge} in the app module;
 * libartemis.so calls into it through JNI.
 *
 * The engine runs on its own thread, so show() posts the dialog to the UI
 * thread and the native caller polls isDone()/resultText()/resultOk() while it
 * is up.
 */
public final class NativeInput {
    private static Activity sActivity;
    private static volatile boolean sDone;
    private static volatile boolean sOk;
    private static volatile String sText = "";

    private NativeInput() {}

    /** Called once from native (ANativeActivity_onCreate) with the Activity. */
    public static void install(Activity activity) {
        sActivity = activity;
    }

    public static boolean isDone() {
        return sDone;
    }

    public static boolean resultOk() {
        return sOk;
    }

    public static String resultText() {
        return sText == null ? "" : sText;
    }

    /** Native entry point: show the modal box.
     *  mode 0 = message-only alert, 1 = yes/no confirm, 2 = text input. */
    public static void show(final String title, final String message,
                            final String def, final int maxLen, final int mode) {
        sDone = false;
        sOk = false;
        sText = "";
        final Activity a = sActivity;
        if (a == null) {
            sDone = true;
            return;
        }
        a.runOnUiThread(new Runnable() {
            @Override
            public void run() {
                AlertDialog.Builder b = new AlertDialog.Builder(a)
                        .setTitle(title == null ? "" : title)
                        .setMessage(message == null ? "" : message);
                if (mode == 2) {
                    final EditText et = new EditText(a);
                    et.setInputType(InputType.TYPE_CLASS_TEXT);
                    et.setText(def == null ? "" : def);
                    et.setSelection(et.getText().length());
                    if (maxLen > 0) {
                        et.setFilters(new InputFilter[]{
                                new InputFilter.LengthFilter(maxLen)});
                    }
                    b.setView(et)
                            .setPositiveButton("OK",
                                    new DialogInterface.OnClickListener() {
                                        @Override
                                        public void onClick(DialogInterface d, int w) {
                                            sText = et.getText().toString();
                                            sOk = true;
                                            sDone = true;
                                        }
                                    })
                            .setNegativeButton("Cancel",
                                    new DialogInterface.OnClickListener() {
                                        @Override
                                        public void onClick(DialogInterface d, int w) {
                                            sDone = true;
                                        }
                                    });
                } else if (mode == 1) {
                    b.setPositiveButton("OK",
                                    new DialogInterface.OnClickListener() {
                                        @Override
                                        public void onClick(DialogInterface d, int w) {
                                            sOk = true;
                                            sDone = true;
                                        }
                                    })
                            .setNegativeButton("Cancel",
                                    new DialogInterface.OnClickListener() {
                                        @Override
                                        public void onClick(DialogInterface d, int w) {
                                            sDone = true;
                                        }
                                    });
                } else {
                    b.setPositiveButton("OK",
                            new DialogInterface.OnClickListener() {
                                @Override
                                public void onClick(DialogInterface d, int w) {
                                    sOk = true;
                                    sDone = true;
                                }
                            });
                }
                b.setOnCancelListener(new DialogInterface.OnCancelListener() {
                            @Override
                            public void onCancel(DialogInterface d) {
                                sDone = true;
                            }
                        })
                        .show();
            }
        });
    }
}
