/* ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is mozilla.org code.
 *
 * The Initial Developer of the Original Code is Mozilla Corporation.
 * Portions created by the Initial Developer are Copyright (C) 2007
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *  Justin Dolske <dolske@mozilla.com> (original author)
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */


const Cc = Components.classes;
const Ci = Components.interfaces;

Components.utils.import("resource://gre/modules/XPCOMUtils.jsm");

function LoginManagerStorage_legacy() { };

LoginManagerStorage_legacy.prototype = {

    classDescription  : "LoginManagerStorage_legacy",
    contractID : "@mozilla.org/login-manager/storage/legacy;1",
    classID : Components.ID("{e09e4ca6-276b-4bb4-8b71-0635a3a2a007}"),
    QueryInterface : XPCOMUtils.generateQI([Ci.nsILoginManagerStorage]),

    __logService : null, // Console logging service, used for debugging.
    get _logService() {
        if (!this.__logService)
            this.__logService = Cc["@mozilla.org/consoleservice;1"].
                                getService(Ci.nsIConsoleService);
        return this.__logService;
    },

    __decoderRing : null,  // nsSecretDecoderRing service
    get _decoderRing() {
        if (!this.__decoderRing)
            this.__decoderRing = Cc["@mozilla.org/security/sdr;1"].
                                 getService(Ci.nsISecretDecoderRing);
        return this.__decoderRing;
    },

    _prefBranch : null,  // Preferences service

    _signonsFile : null,  // nsIFile for "signons2.txt" (or whatever pref is)
    _debug       : false, // mirrors signon.debug


    /*
     * Core datastructures
     *
     * EG: _logins["http://site.com"][0].password
     * EG: _disabledHosts["never.site.com"]
     */
    _logins        : null, 
    _disabledHosts : null,


    /*
     * log
     *
     * Internal function for logging debug messages to the Error Console.
     */
    log : function (message) {
        if (!this._debug)
            return;
        dump("PwMgr Storage: " + message + "\n");
        this._logService.logStringMessage("PwMgr Storage: " + message);
    },




    /* ==================== Public Methods ==================== */




    initWithFile : function(aInputFile, aOutputFile) {
        this._signonsFile = aInputFile;

        this.init();

        if (aOutputFile) {
            this._signonsFile = aOutputFile;
            this._writeFile();
        }
    },

    /*
     * init
     *
     * Initialize this storage component and load stored passwords from disk.
     */
    init : function () {
        this._logins  = {};
        this._disabledHosts = {};

        // Connect to the correct preferences branch.
        this._prefBranch = Cc["@mozilla.org/preferences-service;1"]
                                .getService(Ci.nsIPrefService);
        this._prefBranch = this._prefBranch.getBranch("signon.");
        this._prefBranch.QueryInterface(Ci.nsIPrefBranch2);

        this._debug = this._prefBranch.getBoolPref("debug");

        // Check to see if the internal PKCS#11 token has been initialized.
        // If not, set a blank password.
        var tokenDB = Cc["@mozilla.org/security/pk11tokendb;1"]
                            .getService(Ci.nsIPK11TokenDB);

        var token = tokenDB.getInternalKeyToken();
        if (token.needsUserInit) {
            this.log("Initializing key3.db with default blank password.");
            token.initPassword("");
        }

        var importFile = null;
        // If initWithFile is calling us, _signonsFile is already set.
        if (!this._signonsFile)
            [this._signonsFile, importFile] = this._getSignonsFile();

        // If we have an import file, do a switcharoo before reading it.
        if (importFile) {
            this.log("Importing " + importFile.path);

            var tmp = this._signonsFile;
            this._signonsFile = importFile;
        }

        // Read in the stored login data.
        this._readFile()

        // If we were importing, write back to the normal file.
        if (importFile) {
            this._signonsFile = tmp;
            this._writeFile();
        }
    },


    /*
     * addLogin
     *
     */
    addLogin : function (login) {
        var key = login.hostname;

        // If first entry for key, create an Array to hold it's logins.
        if (!this._logins[key])
            this._logins[key] = [];

        this._logins[key].push(login);

        this._writeFile();
    },


    /*
     * removeLogin
     *
     */
    removeLogin : function (login) {
        var key = login.hostname;
        var logins = this._logins[key];

        if (!logins)
            throw "No logins found for hostname (" + key + ")";

        for (var i = 0; i < logins.length; i++) {
            if (logins[i].equals(login)) {
                logins.splice(i, 1); // delete that login from array.
                break;
                // Note that if there are duplicate entries, they'll
                // have to be deleted one-by-one.
            }
        }

        // Did we delete all the logins for this host?
        if (logins.length == 0)
            delete this._logins[key];

        this._writeFile();
    },


    /*
     * modifyLogin
     *
     */
    modifyLogin : function (oldLogin, newLogin) {
        this.removeLogin(oldLogin);
        this.addLogin(newLogin);
    },


    /*
     * getAllLogins
     *
     * Returns an array of nsAccountInfo.
     */
    getAllLogins : function (count) {
        var result = [];

        // Each entry is an array -- append the array entries to |result|.
        for each (var hostLogins in this._logins) {
            result = result.concat(hostLogins);
        }

        count.value = result.length; // needed for XPCOM
        return result;
    },


    /*
     * clearAllLogins
     *
     * Clears all logins from storage.
     */
    clearAllLogins : function () {
        this._logins = {};
        // Disabled hosts kept, as one presumably doesn't want to erase those.

        this._writeFile();
    },


    /*
     * getAllDisabledHosts
     *
     */
    getAllDisabledHosts : function (count) {
        var result = [];

        for (var hostname in this._disabledHosts) {
            result.push(hostname);
        }

        count.value = result.length; // needed for XPCOM
        return result;
    },


    /*
     * getLoginSavingEnabled
     *
     */
    getLoginSavingEnabled : function (hostname) {
        return !this._disabledHosts[hostname];
    },


    /*
     * setLoginSavingEnabled
     *
     */
    setLoginSavingEnabled : function (hostname, enabled) {
        if (enabled)
            delete this._disabledHosts[hostname];
        else
            this._disabledHosts[hostname] = true;

        this._writeFile();
    },


    /*
     * findLogins
     *
     */
    findLogins : function (count, hostname, formSubmitURL, httpRealm) {
        var hostLogins = this._logins[hostname];
        if (hostLogins == null) {
            count.value = 0;
            return [];
        }

        var result = [];

        for each (var login in hostLogins) {

            // If looking for an HTTP login, make sure the httpRealms match.
            if (httpRealm != login.httpRealm)
                continue;

            // If looking for a form login, make sure the action URLs match 
            // ...unless the stored login is blank (not null), which means
            // login was stored before we started keeping the action URL.
            if (formSubmitURL != login.formSubmitURL &&
                login.formSubmitURL != "")
                continue;

            result.push(login);
        }

        count.value = result.length; // needed for XPCOM
        return result;
    },




    /* ==================== Internal Methods ==================== */




    /*
     * _getSignonsFile
     *
     * Determines what file to use based on prefs. Returns it as a
     * nsILocalFile, along with a file to import from first (if needed)
     *
     */
    _getSignonsFile : function() {
        var importFile = null;

        // Get the location of the user's profile.
        var DIR_SERVICE = new Components.Constructor(
                "@mozilla.org/file/directory_service;1", "nsIProperties");
        var pathname = (new DIR_SERVICE()).get("ProfD", Ci.nsIFile).path;


        // First try the default pref...
        var filename = this._prefBranch.getCharPref("SignonFileName2");

        var file = Cc["@mozilla.org/file/local;1"].
                   createInstance(Ci.nsILocalFile);
        file.initWithPath(pathname);
        file.append(filename);

        if (!file.exists()) {
            this.log("SignonFilename2 file does not exist. file=" +
                     filename + ", path=" + pathname);

            // Then try the old pref...
            var oldname = this._prefBranch.getCharPref("SignonFileName");

            importFile = Cc["@mozilla.org/file/local;1"].
                         createInstance(Ci.nsILocalFile);
            importFile.initWithPath(pathname);
            importFile.append(oldname);

            if (!importFile.exists()) {
                this.log("SignonFilename1 file does not exist. file=" +
                        oldname + ", path=" + pathname);
                importFile = null;
            }
        }

        return [file, importFile];
    },


    /*
     * _readFile
     *
     */
    _readFile : function () {
        var oldFormat = false;
        var writeOnFinish = false;

        this.log("Reading passwords from " + this._signonsFile.path);

        // If it doesn't exist, just create an empty file and bail out.
        if (!this._signonsFile.exists()) {
            this.log("Creating new signons file...");
            this._writeFile();
            return;
        }

        var inputStream = Cc["@mozilla.org/network/file-input-stream;1"]
                                .createInstance(Ci.nsIFileInputStream);
        // init the stream as RD_ONLY, -1 == default permissions.
        inputStream.init(this._signonsFile, 0x01, -1, null);
        var lineStream = inputStream.QueryInterface(Ci.nsILineInputStream);
        var line = { value: "" };

        const STATE = { HEADER : 0, REJECT : 1, REALM : 2,
                        USERFIELD : 3, USERVALUE : 4,
                        PASSFIELD : 5, PASSVALUE : 6, ACTIONURL : 7 };
        var parseState = STATE.HEADER;

        var nsLoginInfo = new Components.Constructor(
                "@mozilla.org/login-manager/loginInfo;1", Ci.nsILoginInfo);
        var processEntry = false;

        do {
            var hasMore = lineStream.readLine(line);

            switch (parseState) {
                // Check file header
                case STATE.HEADER:
                    if (line.value == "#2c") {
                        oldFormat = true;
                    } else if (line.value != "#2d") {
                        this.log("invalid file header (" + line.value + ")");
                        throw "invalid file header in signons file";
                        // We could disable later writing to file, so we
                        // don't clobber whatever it is. ...however, that
                        // would mean corrupt files are not self-healing.
                        return;
                    }
                    parseState++;
                    break;

                // Line is a hostname for which passwords should never be saved.
                case STATE.REJECT:
                    if (line.value == ".") {
                        parseState++;
                        break;
                    }

                    this._disabledHosts[line.value] = true;

                    break;

                // Line is a hostname, saved login(s) will follow
                case STATE.REALM:
                    var hostrealm = line.value;

                    // Format is "http://site.com", with "(some realm)"
                    // appended if it's a HTTP-Auth login.
                    const realmFormat = /^(.+?)( \(.*\))?$/; // XXX .* or .+?
                    var matches = realmFormat.exec(hostrealm);

                    var hostname, httpRealm;
                    if (matches && matches.length == 3) {
                        hostname  = matches[1];
                        httpRealm = matches[2] ?
                                        matches[2].slice(2, -1) : null;
                    } else {
                        if (hostrealm != "") {
                            // Uhoh. This shouldn't happen, but try to deal.
                            this.log("Error parsing host/realm: " + hostrealm);
                        }
                        hostname = hostrealm;
                        httpRealm = null;
                    }

                    parseState++;
                    break;

                // Line is the HTML 'name' attribute for the username field
                // (or "." to indicate end of hostrealm)
                case STATE.USERFIELD:
                    if (line.value == ".") {
                        parseState = STATE.REALM;
                        break;
                    }

                    var entry = new nsLoginInfo();
                    entry.hostname  = hostname;
                    entry.httpRealm = httpRealm;

                    entry.usernameField = line.value;
                    parseState++;
                    break;

                // Line is a username
                case STATE.USERVALUE:
                    entry.username = this._decrypt(line.value);
                    parseState++;
                    break;

                // Line is the HTML 'name' attribute for the password field,
                // with a leading '*' character
                case STATE.PASSFIELD:
                    entry.passwordField = line.value.substr(1);
                    parseState++;
                    break;

                // Line is a password
                case STATE.PASSVALUE:
                    entry.password = this._decrypt(line.value);
                    if (oldFormat) {
                        entry.formSubmitURL = "";
                        processEntry = true;
                        parseState = STATE.USERFIELD;
                    } else {
                        parseState++;
                    }
                    break;

                // Line is the action URL
                case STATE.ACTIONURL:
                    entry.formSubmitURL = line.value;
                    processEntry = true;
                    parseState = STATE.USERFIELD;
                    break;

            }

            if (processEntry) {
                if (entry.username == "" && entry.password == "") {
                    // Discard bogus entry, and update the file when done.
                    writeOnFinish = true;
                } else {
                    if (!this._logins[hostname])
                        this._logins[hostname] = [];
                    this._logins[hostname].push(entry);
                }
                entry = null;
                processEntry = false;
            }
        } while (hasMore);

        lineStream.close();

        if (writeOnFinish)
            this._writeFile();

        return;
    },


    /*
     * _writeFile
     *
     */
    _writeFile : function () {
        function writeLine(data) {
            data += "\r\n";
            outputStream.write(data, data.length);
        }

        this.log("Writing passwords to " + this._signonsFile.path);

        var outputStream = Cc["@mozilla.org/network/safe-file-output-stream;1"]
                                .createInstance(Ci.nsIFileOutputStream);
        outputStream.QueryInterface(Ci.nsISafeOutputStream);

        // WR_ONLY|CREAT|TRUNC
        outputStream.init(this._signonsFile, 0x02 | 0x08 | 0x20, 0600, null);

        // write file version header
        writeLine("#2d");

        // write disabled logins list
        for (var hostname in this._disabledHosts) {
            writeLine(hostname);
        }

        // write end-of-reject-list marker
        writeLine(".");

        for (var hostname in this._logins) {
            function sortByRealm(a,b) {
                a = a.httpRealm;
                b = b.httpRealm;

                if (!a && !b)
                    return  0;

                if (!a || a < b)
                    return -1;

                if (!b || b > a)
                    return  1;

                return 0; // a==b, neither is null
            }

            // Sort logins by httpRealm. This allows us to group multiple
            // logins for the same realm together.
            this._logins[hostname].sort(sortByRealm);


            // write each login known for the host
            var lastRealm = null;
            var firstEntry = true;
            for each (var login in this._logins[hostname]) {

                // If this login is for a new realm, start a new entry.
                if (login.httpRealm != lastRealm || firstEntry) {
                    // end previous entry, if needed.
                    if (!firstEntry)
                        writeLine(".");

                    var hostrealm = login.hostname;
                    if (login.httpRealm)
                        hostrealm += " (" + login.httpRealm + ")";

                    writeLine(hostrealm);
                }

                firstEntry = false;

                var encUsername = this._encrypt(login.username);
                var encPassword = this._encrypt(login.password);

                writeLine((login.usernameField ?  login.usernameField : ""));
                writeLine(encUsername);
                writeLine("*" +
                    (login.passwordField ?  login.passwordField : ""));
                writeLine(encPassword);
                writeLine((login.formSubmitURL ? login.formSubmitURL : ""));

                lastRealm = login.httpRealm;
            }

            // write end-of-host marker
            writeLine(".");
        }

        // [if there were no hosts, no end-of-host marker (".") needed]

        outputStream.finish();
    },


    /*
     * _encrypt
     *
     */
    _encrypt : function (plainText) {
        var converter = Cc["@mozilla.org/intl/scriptableunicodeconverter"]
                             .createInstance(Ci.nsIScriptableUnicodeConverter);
        converter.charset = "UTF-8";
        var plainOctet = converter.ConvertFromUnicode(plainText);
        plainOctet += converter.Finish();
        return this._decoderRing.encryptString(plainOctet);
    },


    /*
     * decrypt
     *
     */
    _decrypt : function (cipherText) {
        var plainText = null;

        try {
            var plainOctet;
            if (cipherText.charAt(0) == '~') {
                // The older file format obscured entries by
                // base64-encoding them. These entries are signaled by a
                // leading '~' character. 
                plainOctet = atob(cipherText.substring(1));
            } else {
                plainOctet = this._decoderRing.decryptString(cipherText);
            }
            var converter = Cc["@mozilla.org/intl/scriptableunicodeconverter"]
                              .createInstance(Ci.nsIScriptableUnicodeConverter);
            converter.charset = "UTF-8";
            plainText = converter.ConvertToUnicode(plainOctet);
        } catch (e) {
            this.log("Failed to decrypt string: " + cipherText);
        }

        return plainText;
    },

}; // end of nsLoginManagerStorage_legacy implementation

var component = [LoginManagerStorage_legacy];
function NSGetModule(compMgr, fileSpec) {
    return XPCOMUtils.generateModule(component);
}
