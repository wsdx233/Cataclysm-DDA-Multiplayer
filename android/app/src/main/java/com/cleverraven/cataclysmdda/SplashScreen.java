package com.cleverraven.cataclysmdda;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileReader;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.List;
import java.util.Timer;
import java.util.TimerTask;

import android.app.Activity;
import android.app.Dialog;
import android.app.ProgressDialog;
import android.app.AlertDialog;
import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.content.DialogInterface;
import android.content.DialogInterface.OnShowListener;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.*;
import android.preference.PreferenceManager;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.text.InputType;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import com.cleverraven.cataclysmdda.CataclysmDDA_Helpers;

public class SplashScreen extends Activity {
    private static final String TAG = "Splash";
    private static final int INSTALL_DIALOG_ID = 0;
    private static final String PREF_MULTIPLAYER_ENDPOINT = "Multiplayer endpoint";
    private static final String PREF_MULTIPLAYER_TOKEN_ENDPOINT =
        "Multiplayer token endpoint";
    private ProgressDialog installDialog;

    private AlertDialog accessibilityServicesAlert;
    private AlertDialog crashAlert;
    private AlertDialog launchModeAlert;
    private AlertDialog multiplayerConnectAlert;

    public boolean[] mSettingsValues = { false, true, true };
    private int mSystemUiModeIndex = 0;

    private String getVersionName() {
        try {
            Context context = getApplicationContext();
            PackageInfo pInfo = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            return pInfo.versionName;
        } catch (Exception e) {
            e.printStackTrace();
            return "error";
        }
    }

    private void showCrashAlert() {
        if (isFinishing() || (crashAlert != null && crashAlert.isShowing())) {
            return;
        }
        String externalFilesDir = getExternalFilesDir(null).getPath();
        File crashAlertPrompt = new File(externalFilesDir + "/config/crash.log.prompt");
        try {
            crashAlertPrompt.delete();
            if(crashAlertPrompt.exists()) { // Sometimes .delete() doesn't really delete the file and I don't know why
                crashAlertPrompt.getCanonicalFile().delete();
            }
        } catch(IOException e) {
            return;
        }
        File crashLog = new File(externalFilesDir + "/config/crash.log");
        StringBuilder text = new StringBuilder();
        text.append(getString(R.string.crashMessage));
        text.append("\n\n");
        try {
            BufferedReader br = new BufferedReader(new FileReader(crashLog));
            String line;
            while((line = br.readLine()) != null) {
                text.append(line);
                text.append("\n");
            }
            br.close();
        } catch (IOException e) {
            return;
        }
        final String message = text.toString();
        final AlertDialog errorAlert = new AlertDialog.Builder(SplashScreen.this)
            .setTitle(getString(R.string.crashAlert))
            .setCancelable(false)
            .setMessage(message)
            .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface dialog, int id) {
                    SplashScreen.this.showLaunchModeDialog();
                }
            }).create();
        crashAlert = errorAlert;
        errorAlert.setOnDismissListener(dialog -> {
            if (crashAlert == errorAlert) {
                crashAlert = null;
            }
        });
        errorAlert.show();
    }

    @Override
    protected void onStart() {
        Log.e(TAG, "onStart()");
        super.onStart();
    }

    @Override
    protected void onPause() {
        Log.e(TAG, "onPause()");
        super.onPause();
        if (accessibilityServicesAlert != null) {
            accessibilityServicesAlert.dismiss();
        }
    }

    @Override
    protected void onResume() {
        Log.e(TAG, "onResume()");
        super.onResume();

        if ((crashAlert != null && crashAlert.isShowing()) ||
            (launchModeAlert != null && launchModeAlert.isShowing()) ||
            (multiplayerConnectAlert != null && multiplayerConnectAlert.isShowing())) {
            return;
        }

        Context context = getApplicationContext();
        String service_names = CataclysmDDA_Helpers.getEnabledAccessibilityServiceNames(context);
        accessibilityServicesAlert.setMessage( String.format( getString(R.string.accessibilityServicesMessage), service_names ) );
        if (!service_names.isEmpty()) {
            accessibilityServicesAlert.show();
        } else {
            SplashScreen.this.installOrRun();
        }
    }
    
    protected void installOrRun() {
        Log.e(TAG, "onCreate()");
        accessibilityServicesAlert.dismiss();
        // Start the game if already installed, otherwise start installing...
        if (getVersionName().equals(PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).getString("installed", ""))) {
            // Show an alert box if the game crashed last time
            String externalFilesDir = getExternalFilesDir(null).getPath();
            File crashAlertPrompt = new File(externalFilesDir + "/config/crash.log.prompt");
            if(crashAlertPrompt.exists()) {
                showCrashAlert();
            } else {
                showLaunchModeDialog();
            }
        }
        else {
            new InstallProgramTask().execute();
        }
        return;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.e(TAG, "onCreate()");
        super.onCreate(savedInstanceState);

        accessibilityServicesAlert = new AlertDialog.Builder(SplashScreen.this)
            .setTitle(getString(R.string.accessibilityServicesTitle))
            .setCancelable(false)
            .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface dialog, int id) {
                    SplashScreen.this.installOrRun();
                    return;
                }
            })
            .setNeutralButton(getString(R.string.showAccessibilitySettings), new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        startActivityForResult(new Intent(android.provider.Settings.ACTION_ACCESSIBILITY_SETTINGS), 0);
                        dialog.dismiss();
                        return;
                    }
            })
            .setNegativeButton(getString(R.string.ignoreFalsePostives), new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        CataclysmDDA_Helpers.saveAccessibilityServiceInfoFalsePositives(getApplicationContext());
                        SplashScreen.this.installOrRun();
                        return;
                    }
            }).create();
    }

    @Override
    public Dialog onCreateDialog(int id) {
        switch (id) {
            case INSTALL_DIALOG_ID:
                installDialog = new ProgressDialog(this);
                installDialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL);
                boolean clean_install = PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).getString("installed", "").isEmpty();
                installDialog.setTitle(getString(clean_install ? R.string.installTitle : R.string.upgradeTitle));
                installDialog.setIndeterminate(true);
                installDialog.setCancelable(false);
                return installDialog;
            default:
                return null;
        }
    }

    private File getMultiplayerTokenFile() throws IOException {
        File directory = new File(getNoBackupFilesDir(), "multiplayer");
        if ((!directory.exists() && !directory.mkdirs()) || !directory.isDirectory()) {
            throw new IOException(getString(R.string.multiplayerTokenStorageError));
        }
        File tokenFile = new File(directory, "client-token.txt");
        String directoryPath = directory.getCanonicalPath() + File.separator;
        if (!tokenFile.getCanonicalPath().startsWith(directoryPath)) {
            throw new IOException(getString(R.string.multiplayerTokenStorageError));
        }
        return tokenFile;
    }

    private boolean isValidMultiplayerToken(String token) {
        return token != null && token.matches("[0-9a-f]{64}");
    }

    private String loadSavedMultiplayerToken() {
        try {
            File tokenFile = getMultiplayerTokenFile();
            if (!tokenFile.isFile() || tokenFile.length() > 128) {
                return "";
            }
            BufferedReader reader = new BufferedReader(new FileReader(tokenFile));
            String token = reader.readLine();
            reader.close();
            return isValidMultiplayerToken(token) ? token : "";
        } catch (IOException error) {
            return "";
        }
    }

    private File saveMultiplayerToken(String token) throws IOException {
        File tokenFile = getMultiplayerTokenFile();
        File temporaryFile = new File(tokenFile.getParentFile(), "client-token.tmp");
        if (temporaryFile.exists() && !temporaryFile.delete()) {
            throw new IOException(getString(R.string.multiplayerTokenStorageError));
        }
        FileOutputStream output = new FileOutputStream(temporaryFile, false);
        try {
            output.write((token + "\n").getBytes("UTF-8"));
            output.flush();
            output.getFD().sync();
        } finally {
            output.close();
        }
        try {
            Os.chmod(temporaryFile.getAbsolutePath(),
                OsConstants.S_IRUSR | OsConstants.S_IWUSR);
            Os.rename(temporaryFile.getAbsolutePath(), tokenFile.getAbsolutePath());
            FileDescriptor directoryDescriptor = Os.open(
                tokenFile.getParentFile().getAbsolutePath(),
                OsConstants.O_RDONLY, 0);
            try {
                Os.fsync(directoryDescriptor);
            } finally {
                Os.close(directoryDescriptor);
            }
        } catch (ErrnoException error) {
            temporaryFile.delete();
            throw new IOException(getString(R.string.multiplayerTokenStorageError), error);
        }
        return tokenFile;
    }

    private void showLaunchModeDialog() {
        if (isFinishing() || (launchModeAlert != null && launchModeAlert.isShowing())) {
            return;
        }
        launchModeAlert = new AlertDialog.Builder(SplashScreen.this)
            .setTitle(getString(R.string.launchModeTitle))
            .setMessage(getString(R.string.launchModeMessage))
            .setCancelable(false)
            .setPositiveButton(getString(R.string.startSinglePlayer),
                new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        startGameActivity(false);
                    }
                })
            .setNegativeButton(getString(R.string.connectMultiplayer),
                new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        showMultiplayerConnectDialog();
                    }
                })
            .create();
        launchModeAlert.show();
    }

    private void showMultiplayerConnectDialog() {
        if (isFinishing() ||
            (multiplayerConnectAlert != null && multiplayerConnectAlert.isShowing())) {
            return;
        }
        SharedPreferences preferences =
            PreferenceManager.getDefaultSharedPreferences(getApplicationContext());
        String savedEndpoint = preferences.getString(PREF_MULTIPLAYER_ENDPOINT, "");
        String tokenEndpoint = preferences.getString(PREF_MULTIPLAYER_TOKEN_ENDPOINT, "");
        String savedToken = loadSavedMultiplayerToken();
        LinearLayout layout = new LinearLayout(SplashScreen.this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = (int)(24 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, 0, padding, 0);

        TextView securityNotice = new TextView(SplashScreen.this);
        securityNotice.setText(getString(R.string.multiplayerSecurityNotice));
        layout.addView(securityNotice);

        EditText endpointInput = new EditText(SplashScreen.this);
        endpointInput.setSingleLine(true);
        endpointInput.setHint(getString(R.string.multiplayerEndpointHint));
        endpointInput.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        endpointInput.setText(savedEndpoint);
        layout.addView(endpointInput);

        EditText tokenInput = new EditText(SplashScreen.this);
        tokenInput.setSingleLine(true);
        tokenInput.setHint(!savedToken.isEmpty() && savedEndpoint.equals(tokenEndpoint)
            ? getString(R.string.multiplayerSavedTokenHint)
            : getString(R.string.multiplayerTokenHint));
        tokenInput.setInputType(InputType.TYPE_CLASS_TEXT |
            InputType.TYPE_TEXT_VARIATION_PASSWORD);
        layout.addView(tokenInput);

        CheckBox allowInsecureLan = new CheckBox(SplashScreen.this);
        allowInsecureLan.setText(getString(R.string.multiplayerAllowInsecureLan));
        // This is a per-connection security exception.  Never carry consent from a
        // previous endpoint into a later launch.
        allowInsecureLan.setChecked(false);
        layout.addView(allowInsecureLan);

        AlertDialog connectDialog = new AlertDialog.Builder(SplashScreen.this)
            .setTitle(getString(R.string.multiplayerConnectTitle))
            .setView(layout)
            .setCancelable(false)
            .setPositiveButton(getString(R.string.connectMultiplayer), null)
            .setNegativeButton(android.R.string.cancel, new DialogInterface.OnClickListener() {
                public void onClick(DialogInterface dialog, int id) {
                    showLaunchModeDialog();
                }
            })
            .create();
        multiplayerConnectAlert = connectDialog;
        connectDialog.setOnDismissListener(dialog -> {
            if (multiplayerConnectAlert == connectDialog) {
                multiplayerConnectAlert = null;
            }
        });
        connectDialog.setOnShowListener(new OnShowListener() {
            @Override
            public void onShow(DialogInterface ignored) {
                connectDialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(view -> {
                    String endpoint = endpointInput.getText().toString().trim();
                    String token = tokenInput.getText().toString().trim();
                    if (endpoint.isEmpty() || endpoint.length() > 512 ||
                        endpoint.indexOf(':') < 0) {
                        endpointInput.setError(getString(R.string.multiplayerEndpointInvalid));
                        return;
                    }
                    if (token.isEmpty()) {
                        if (!endpoint.equals(tokenEndpoint) || savedToken.isEmpty()) {
                            tokenInput.setError(getString(
                                R.string.multiplayerTokenEndpointChanged));
                            return;
                        }
                        token = savedToken;
                    }
                    if (!isValidMultiplayerToken(token)) {
                        tokenInput.setError(getString(R.string.multiplayerTokenInvalid));
                        return;
                    }
                    try {
                        File tokenFile = saveMultiplayerToken(token);
                        preferences.edit()
                            .putString(PREF_MULTIPLAYER_ENDPOINT, endpoint)
                            .putString(PREF_MULTIPLAYER_TOKEN_ENDPOINT, endpoint)
                            .apply();
                        Intent intent = new Intent(SplashScreen.this, CataclysmDDA.class);
                        intent.putExtra(CataclysmDDA.EXTRA_MULTIPLAYER_MODE, true);
                        intent.putExtra(CataclysmDDA.EXTRA_MULTIPLAYER_ENDPOINT, endpoint);
                        intent.putExtra(CataclysmDDA.EXTRA_MULTIPLAYER_TOKEN_FILE,
                            tokenFile.getAbsolutePath());
                        intent.putExtra(CataclysmDDA.EXTRA_MULTIPLAYER_ALLOW_INSECURE_LAN,
                            allowInsecureLan.isChecked());
                        connectDialog.dismiss();
                        startGameActivity(false, intent);
                    } catch (IOException error) {
                        Toast.makeText(SplashScreen.this, error.getMessage(),
                            Toast.LENGTH_LONG).show();
                    }
                });
            }
        });
        connectDialog.show();
    }

    private void startGameActivity(boolean delay) {
        startGameActivity(delay, new Intent(SplashScreen.this, CataclysmDDA.class));
    }

    private void startGameActivity(boolean delay, final Intent intent) {
        intent.addFlags(Intent.FLAG_ACTIVITY_NO_ANIMATION);
        if (!delay) {
            runOnUiThread(new StartGameRunnable(intent));
        }
        else {
            // Wait 1.5 seconds, then start game
            Timer timer = new Timer();
            TimerTask gameStartTask = new TimerTask() {
                @Override
                public void run() {
                    runOnUiThread(new StartGameRunnable(intent));
                }
            };
            timer.schedule(gameStartTask, 1500);
        }
    }

    private final class StartGameRunnable implements Runnable {
        private final Intent intent;

        StartGameRunnable(Intent intent) {
            this.intent = intent;
        }

        @Override
        public void run() {
            startActivity(intent);
            finish();
            overridePendingTransition(0, 0);
        }
    }

    private class InstallProgramTask extends AsyncTask<Void, Integer, Boolean> {
        private final List<String> PRESERVE_SUBFOLDERS = Arrays.asList("sound", "mods", "gfx"); // don't delete custom subfolders under these folders
        private final List<String> PRESERVE_FOLDERS = Arrays.asList("font"); // don't delete this folder
        private final List<String> PRESERVE_FILES = Arrays.asList("user-default-mods.json"); // don't delete this file

        private int totalFiles = 0;
        private int installedFiles = 0;

        private AlertDialog installationAlert;
        private AlertDialog settingsAlert;
        private AlertDialog helpAlert;

        @Override
        protected void onPreExecute() {
            installationAlert = new AlertDialog.Builder(SplashScreen.this)
                .setTitle("Installation Failed")
                .setCancelable(false)
                .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        SplashScreen.this.finish();
                        return;
                    }
                }).create();
            AssetManager assetManager = getAssets();
            try {
                totalFiles = countTotalAssets(assetManager, "data") +
                    countTotalAssets(assetManager, "gfx") +
                    countTotalAssets(assetManager, "lang");
                showDialog(INSTALL_DIALOG_ID);
            } catch(Exception e) {
                installationAlert.setMessage(e.getMessage());
                installationAlert.show();
            }

            helpAlert = new AlertDialog.Builder(SplashScreen.this)
                .setTitle(getString(R.string.helpTitle))
                .setCancelable(false)
                .setMessage(getString(R.string.helpMessage))
                .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        settingsAlert.show();
                        return;
                    }
                }).create();

            loadSettingsDefaults();

            settingsAlert = new AlertDialog.Builder(SplashScreen.this)
                .setTitle(getString(R.string.settings))
                .setView(createSettingsView())
                .setCancelable(false)
                .setPositiveButton(getString(R.string.startGame), new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).edit().putBoolean("Software rendering", SplashScreen.this.mSettingsValues[0]).commit();
                        PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).edit().putString(CataclysmDDA.PREF_SYSTEM_UI_MODE, getSelectedSystemUiMode()).commit();
                        PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).edit().putBoolean("Trap Back button", SplashScreen.this.mSettingsValues[1]).commit();
                        PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).edit().putBoolean("Native Android UI", SplashScreen.this.mSettingsValues[2]).commit();
                        SplashScreen.this.showLaunchModeDialog();
                        return;
                    }
                })
                .setNeutralButton(getString(R.string.showHelp), new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        helpAlert.show();
                        return;
                    }
                }).create();
        }

        private void loadSettingsDefaults() {
            SharedPreferences preferences = PreferenceManager.getDefaultSharedPreferences(getApplicationContext());
            SplashScreen.this.mSettingsValues[0] = preferences.getBoolean("Software rendering", false);
            SplashScreen.this.mSettingsValues[1] = preferences.getBoolean("Trap Back button", true);
            SplashScreen.this.mSettingsValues[2] = preferences.getBoolean("Native Android UI", true);

            String mode;
            if (preferences.contains(CataclysmDDA.PREF_SYSTEM_UI_MODE)) {
                mode = preferences.getString(
                    CataclysmDDA.PREF_SYSTEM_UI_MODE,
                    CataclysmDDA.SYSTEM_UI_MODE_SYSTEM_BARS);
            } else {
                mode = preferences.getBoolean(CataclysmDDA.PREF_FORCE_FULLSCREEN, false)
                    ? CataclysmDDA.SYSTEM_UI_MODE_EDGE_TO_EDGE
                    : CataclysmDDA.SYSTEM_UI_MODE_SYSTEM_BARS;
            }
            SplashScreen.this.mSystemUiModeIndex = systemUiModeIndex(mode);
        }

        private ScrollView createSettingsView() {
            ScrollView scrollView = new ScrollView(SplashScreen.this);
            LinearLayout layout = new LinearLayout(SplashScreen.this);
            layout.setOrientation(LinearLayout.VERTICAL);
            int padding = (int)(24 * getResources().getDisplayMetrics().density);
            layout.setPadding(padding, 0, padding, 0);

            TextView displayModeLabel = new TextView(SplashScreen.this);
            displayModeLabel.setText(getString(R.string.androidSystemUiMode));
            layout.addView(displayModeLabel);

            RadioGroup displayModeGroup = new RadioGroup(SplashScreen.this);
            displayModeGroup.setOrientation(RadioGroup.VERTICAL);
            addSystemUiModeButton(displayModeGroup, 0, getString(R.string.androidSystemUiModeSystemBars));
            addSystemUiModeButton(displayModeGroup, 1, getString(R.string.androidSystemUiModeFullscreen));
            addSystemUiModeButton(displayModeGroup, 2, getString(R.string.androidSystemUiModeEdgeToEdge));
            displayModeGroup.check(systemUiModeButtonId(mSystemUiModeIndex));
            displayModeGroup.setOnCheckedChangeListener(new RadioGroup.OnCheckedChangeListener() {
                @Override
                public void onCheckedChanged(RadioGroup group, int checkedId) {
                    SplashScreen.this.mSystemUiModeIndex = systemUiModeIndexFromButtonId(checkedId);
                }
            });
            layout.addView(displayModeGroup);

            addBooleanSetting(layout, 0, getString(R.string.softwareRendering));
            addBooleanSetting(layout, 1, getString(R.string.trapBackButton));
            addBooleanSetting(layout, 2, getString(R.string.nativeAndroidUI));
            scrollView.addView(layout, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
            return scrollView;
        }

        private void addSystemUiModeButton(RadioGroup group, int id, String label) {
            RadioButton button = new RadioButton(SplashScreen.this);
            button.setId(systemUiModeButtonId(id));
            button.setText(label);
            group.addView(button, new RadioGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        private int systemUiModeButtonId(int index) {
            return 1000 + index;
        }

        private int systemUiModeIndexFromButtonId(int buttonId) {
            return buttonId - 1000;
        }

        private void addBooleanSetting(LinearLayout layout, final int index, String label) {
            CheckBox checkBox = new CheckBox(SplashScreen.this);
            checkBox.setText(label);
            checkBox.setChecked(SplashScreen.this.mSettingsValues[index]);
            checkBox.setOnClickListener(v -> SplashScreen.this.mSettingsValues[index] = checkBox.isChecked());
            layout.addView(checkBox, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        }

        private String getSelectedSystemUiMode() {
            switch (mSystemUiModeIndex) {
                case 1:
                    return CataclysmDDA.SYSTEM_UI_MODE_FULLSCREEN;
                case 2:
                    return CataclysmDDA.SYSTEM_UI_MODE_EDGE_TO_EDGE;
                default:
                    return CataclysmDDA.SYSTEM_UI_MODE_SYSTEM_BARS;
            }
        }

        private int systemUiModeIndex(String mode) {
            if (CataclysmDDA.SYSTEM_UI_MODE_FULLSCREEN.equals(mode)) {
                return 1;
            } else if (CataclysmDDA.SYSTEM_UI_MODE_EDGE_TO_EDGE.equals(mode)) {
                return 2;
            }
            return 0;
        }

        @Override
        protected Boolean doInBackground(Void... params) {
            if (installDialog != null) {
                installDialog.setIndeterminate(false);
                installDialog.setMax(totalFiles);
            }
            publishProgress(installedFiles);

            AssetManager assetManager = getAssets();
            String externalFilesDir = getExternalFilesDir(null).getPath();

            try {
                // Clear out the old data if it exists (but preserve custom folders + files)
                deleteRecursive(assetManager, externalFilesDir, new File(externalFilesDir + "/data"));
                deleteRecursive(assetManager, externalFilesDir, new File(externalFilesDir + "/gfx"));
                deleteRecursive(assetManager, externalFilesDir, new File(externalFilesDir + "/lang"));

                // Install the new data over the top
                copyAssetFolder(assetManager, "data", externalFilesDir + "/data");
                copyAssetFolder(assetManager, "gfx", externalFilesDir + "/gfx");
                copyAssetFolder(assetManager, "lang", externalFilesDir + "/lang");
            } catch(Exception e) {
                installationAlert.setMessage(e.getMessage());
                return false;
            }

            // Remember which version the installed data is
            PreferenceManager.getDefaultSharedPreferences(getApplicationContext()).edit().putString("installed", getVersionName()).commit();

            publishProgress(++installedFiles);
            Log.d(TAG, "Total number of files copied: " + installedFiles);
            return true;
        }

        void deleteRecursive(AssetManager assetManager, String externalFilesDir, File fileOrDirectory) {
            String parentFolder = fileOrDirectory.getParentFile().getName().toLowerCase();
            String fileOrDirectoryName = fileOrDirectory.getName().toLowerCase();
            if (fileOrDirectory.isDirectory()) {
                // Don't delete the folder if it is in the preserve folders list
                if (PRESERVE_FOLDERS.contains(fileOrDirectoryName))
                    return;

                // Don't delete the folder if its parent is in the preserve subfolders list, and it doesn't exist in the APK assets (so must be custom data)
                if (PRESERVE_SUBFOLDERS.contains(parentFolder) && !assetExists(assetManager, fileOrDirectory.getPath().substring(externalFilesDir.length()+1)))
                    return;

                for (File child : fileOrDirectory.listFiles())
                    deleteRecursive(assetManager, externalFilesDir, child);
            }
            else {
                // Don't delete the file if it's in the preserve files list
                if (PRESERVE_FILES.contains(fileOrDirectoryName))
                    return;
            }

            fileOrDirectory.delete();
        }

        // Returns true if an asset exists in the APK (either a directory or a file)
        // eg. assetExists("data/sound") or assetExists("data/font", "unifont.ttf") would both return true
        private boolean assetExists(AssetManager assetManager, String assetPath) {
            return assetExists(assetManager, assetPath, "");
        }

        private boolean assetExists(AssetManager assetManager, String assetPath, String assetName) {
            try {
                String[] files = assetManager.list(assetPath);
                if (assetName.isEmpty())
                    return files.length > 0; // folder exists
                for (String file : files) {
                    if (file.equalsIgnoreCase(assetName))
                        return true; // file exists
                }
                return false;
            } catch (Exception e) {
                e.printStackTrace();
                return false;
            }
        }

        private int countTotalAssets(AssetManager assetManager, String assetPath) throws Exception {
            try {
                String[] files = assetManager.list(assetPath);
                int count = 0;
                for (String file : files)
                {
                    String filePath = assetPath + "/" + file;
                    String subdir_files[] = assetManager.list(filePath);
                    if (subdir_files.length == 0) // file
                        count++;
                    else // folder
                        count += countTotalAssets(assetManager, filePath);
                }
                return count;
            } catch (Exception e) {
                e.printStackTrace();
                throw e;
            }
        }

        // Pinched from http://stackoverflow.com/questions/16983989/copy-directory-from-assets-to-data-folder
        private boolean copyAssetFolder(AssetManager assetManager, String fromAssetPath, String toPath) throws Exception {
            try {
                String[] files = assetManager.list(fromAssetPath);
                new File(toPath).mkdirs();
                boolean res = true;
                for (String file : files)
                {
                    String subdir_files[] = assetManager.list(fromAssetPath + "/" + file);
                    if (subdir_files.length == 0) // file
                        res &= copyAsset(assetManager, fromAssetPath + "/" + file, toPath + "/" + file);
                    else // folder
                        res &= copyAssetFolder(assetManager, fromAssetPath + "/" + file, toPath + "/" + file);
                }
                return res;
            } catch (Exception e) {
                e.printStackTrace();
                throw e;
            }
        }

        private boolean copyAsset(AssetManager assetManager, String fromAssetPath, String toPath) throws Exception {
            publishProgress(++installedFiles, totalFiles);
            InputStream in = null;
            OutputStream out = null;
            try {
              in = assetManager.open(fromAssetPath);
              new File(toPath).createNewFile();
              out = new FileOutputStream(toPath);
              copyFile(in, out);
              in.close();
              in = null;
              out.flush();
              out.close();
              out = null;
              return true;
            } catch(Exception e) {
                e.printStackTrace();
                throw e;
            }
        }

        private void copyFile(InputStream in, OutputStream out) throws IOException {
            byte[] buffer = new byte[1024];
            int read;
            while((read = in.read(buffer)) != -1) {
              out.write(buffer, 0, read);
            }
        }

        @Override
        protected void onProgressUpdate(Integer... values) {
            if (installDialog == null) {
                return;
            }
            installDialog.setProgress(values[0]);
        }

        @Override
        protected void onPostExecute(Boolean result) {
            removeDialog(INSTALL_DIALOG_ID);
            if(result) {
                settingsAlert.show();
            } else {
                installationAlert.show();
            }
        }
    }
}
