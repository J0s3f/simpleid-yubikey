<?php
/*
 * SimpleID
 *
 * Copyright (C) Kelvin Mo 2007-9
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 * 
 * $Id$
 */

/**
 * Functions for persistent storage via the file system.
 *
 * In general, there are three different sets of data which SimpleID needs
 * to store:
 *
 * - transient data (e.g. OpenID associations, sessions, auto-login)
 * - application data (e.g. salt for form tokens)
 * - user data (e.g. user names, passwords and settings)
 *
 * Prior to version 0.7, both transient data and application data are stored
 * using {@link cache.inc}.  From version 0.7, application data are now
 * stored separately from the cache.
 *
 * Prior to version 0.7, user data is only stored in the identity file, to which
 * SimpleID cannot write.  This means that if the user wishes to change a setting,
 * he or she will need to edit the identity file manually.  Other user settings
 * (e.g. RP preferences) are stored using {@link cache.inc}
 *
 * From version 0.7, user data is stored in two files, one is the identity
 * file, the other is the user store file, which SimpleID can write.
 *
 * @package simpleid
 * @filesource
 */

require_once 'Auth/Yubico.php';

/**
 * This variable is a cache of SimpleID's application settings.  It is populated
 * progressively as {@link store_get()} is called.
 *
 * @global array $simpleid_settings
 */
$simpleid_settings = array();

/**
 * Returns whether the user name exists in the user store.
 *
 * @param string $uid the name of the user to check
 * @param bool whether the user name exists
 */
function store_user_exists($uid) {
    if (_store_is_valid_name($uid)) {
        $identity_file = SIMPLEID_IDENTITIES_DIR . "/$uid.identity";
        return (file_exists($identity_file));
    } else {
        return false;
    }
}

/**
 * Loads user data for a specified user name.
 *
 * The user name must exist.  You should check whether the user name exists with
 * the {@link store_user_exists()} function
 *
 * @param string $uid the name of the user to load
 * @return mixed data for the specified user
 */
function store_user_load($uid) {
    if (!_store_is_valid_name($uid)) return array();
    $store_file = SIMPLEID_STORE_DIR . "/$uid.usrstore";
    
    if (file_exists($store_file)) {
        $data = unserialize(file_get_contents($store_file));
    } else {
        $data = array();
    }
    
    $identity_file = SIMPLEID_IDENTITIES_DIR . "/$uid.identity";
    $data = array_merge($data, parse_ini_file($identity_file, TRUE));
    
    return $data;
}

/**
 * Verifies a set of credentials for a specified user.
 *
 * A set of credentials comprises:
 *
 * - A user name
 * - Some kind of verifying information, such as a plaintext password, a hashed
 *   password (e.g. digest) or some other kind of identifying information.
 *
 * The user name is passed to this function using the $uid parameter.  The user
 * name may or may not exist.  If the user name does not exist, this function
 * <strong>must</strong> return false.
 *
 * The credentials are supplied as an array using the $credentials parameter.
 * Typically this array will be a subset of the $_POST superglobal passed to the
 * {@link user_login()} function.  Thus it will generally contain the keys 'pass' and
 * 'digest'.
 *
 * This function must check whether the credentials supplied matches the credentials
 * for the specified user in the store.  If for any reason that credentials
 * do not match, this function <strong>must</strong> return false.
 *
 * @param string $uid the name of the user to verify
 * @param array $credentials the credentials supplied by the browser
 * @param bool $allow_legacy_login true to verify using a plaintext password instead of
 * digest
 * @return bool whether the credentials supplied matches those for the specified
 * user
 */
function store_user_verify_credentials($uid, $credentials, $allow_legacy_login = FALSE) {
    $test_user = user_load($uid);
    
    if ($test_user == NULL) return false;
    
	if (!isset($test_user['auth_method']) || $test_user['auth_method'] === 'STATIC') {
		return filesystem_user_verify_static_password($test_user, $credentials);
	} elseif ($test_user['auth_method'] === 'YUBIKEY') {
		return filesystem_user_verify_yubikey($test_user, $credentials);
	}
    
    return false;
}

/**
 * Verifies a user who relies on a statically defined password in the identity file.
 * @param array $test_user the user
 * @param array $credentials the credentials supplied by the browser
 */
function filesystem_user_verify_static_password($test_user, $credentials, $allow_legacy_login = false) {
	if ($credentials['digest'] && !_user_verify_digest($credentials['digest'], $credentials['nonce'], $test_user)) {
		return false;
	} elseif ($allow_legacy_login && !$credentials['digest'] && (md5($credentials['pass'] != $test_user['pass']))) {
		return false;
	}

	return true;
}

/**
 * Verifies a user who relies on a Yubico Yubikey to authenticate.
 * @param array $test_user the user
 * @param array $credentials the credentials supplied by the browser
 */
function filesystem_user_verify_yubikey($test_user, $credentials) {
	// check for required settings in the identity file
	if (!isset($test_user['yubikey']) || !is_array($test_user['yubikey'])) {
		log_warn('auth_method method for ' . $test_user['uid'] . ' is YUBIKEY, but the yubikey section is missing ' .
			'from the identity file.');
		return false;
	}

	$yubi_user = &$test_user['yubikey'];
	if (   !isset($yubi_user['client_id'])
		|| !isset($yubi_user['client_key'])
		|| !isset($yubi_user['use_https'])
		|| !isset($yubi_user['key_id'])) {
		log_warn('auth_method for ' . $test_user['uid'] . ' is YUBIKEY, but at least one of the client_id, client_key, ' .
			'use_https, or key_id settings are missing from the yubikey section of the identity file.');
		return false;
	}

	// check for the yubikey OTP
	if (!isset($credentials['pass'])) {
		log_debug('auth_method for ' . $test_user['uid'] . ' is YUBIKEY, but no yubikey OTP was sent.');
		return false;
	}

	// create the verification class
	$yubi = new Auth_Yubico($yubi_user['client_id'], $yubi_user['client_key'], $yubi_user['use_https'] ? 1 : 0);

	// add custom URLs if the identity files contains any (HTTP/HTTPS is determined by the 
	// user_https parameter). The library will fall back to the official Yubico servers if this 
	// isn't set.
	if (isset($yubi_user['URLs']) && is_array($yubi_user['URLs'])) {
		foreach ($yubi_user['URLs'] as $url) {
			$yubi->addURLpart($url);
		}
	}

	// authenticate against the verification server
	$auth = $yubi->verify($credentials['pass']);
	if (PEAR::isError($auth)) {
		log_debug('authentication against yubikey server for user ' . $test_user['uid'] . ' failed.');
		return false;
	}

	// verify that the given Yubikey is actually allowed to authenticate for this user. Don't do 
	// this before sending the OTP to the verification server to make sure replay attacks are not 
	// possible with the OTP given for this attempt.
	$parts = $yubi->parsePasswordOTP($credentials['pass']);
	if ($parts === false) {
		log_debug('authentication for user ' . $test_user['uid'] . ' failed because the OTP doesn\'t look like one.');
		return false;
	}
	if ($yubi_user['key_id'] !== $parts['prefix']) {
		log_debug('Yubikey authentication for user ' . $test_user['uid'] . ' expects key prefix ' .
			$yubi_user['key_id'] . ', but got ' . $parts['prefix']);
		return false;
	}

	return true;
}


/**
 * Finds the user name from a specified OpenID Identity URI.
 *
 * @param string $identity the Identity URI of the user to load
 * @return string the user name for the Identity URI, or NULL if no user has
 * the specified Identity URI
 */
function store_get_uid($identity) {
    $uid = cache_get('identity', $identity);
    if ($uid !== NULL) return $uid;
    
    $r = NULL;
    
    $dir = opendir(SIMPLEID_IDENTITIES_DIR);
    
    while (($file = readdir($dir)) !== false) {
        $filename = SIMPLEID_IDENTITIES_DIR . '/' . $file;
        
        if ((filetype($filename) != "file") || (!preg_match('/^(.+)\.identity$/', $file, $matches))) continue;
        
        $uid = $matches[1];
        $test_user = store_user_load($uid);
        
        cache_set('identity', $test_user['identity'], $uid);
    
        if ($test_user['identity'] == $identity) {
            $r = $uid;
        }
    }
        
    closedir($dir);
    
    return $r;
}

/**
 * Saves user data for a specific user name.
 *
 * This data is stored in the user store file.
 *
 * @param string $uid the name of the user
 * @param array $data the data to save
 * @param array $exclude an array of keys to exclude from the user store file.
 * These are generally keys which are stored in the identity file.
 *
 * @since 0.7
 */
function store_user_save($uid, $data, $exclude = array()) {
    foreach ($exclude as $key) {
        if (isset($data[$key])) unset($data[$key]);
    }
    
    if (!_store_is_valid_name($uid)) {
        trigger_error("Invalid user name for filesystem store", E_USER_ERROR);
        return;
    }
    
    $store_file = SIMPLEID_STORE_DIR . "/$uid.usrstore";
    $file = fopen($store_file, 'w');
    fwrite($file, serialize($data));
    fclose($file);
}

/**
 * Loads an application setting.
 *
 * @param string $name the name of the setting to return
 * @param mixed $default the default value to use if this variable has never been set
 * @return mixed the value of the setting
 *
 */
function store_get($name, $default = NULL) {
    global $simpleid_settings;
    
    if (!_store_is_valid_name($name)) return $default;
    
    if (!isset($simpleid_settings[$name])) {
        $setting_file = SIMPLEID_STORE_DIR . "/$name.setting";
        
        if (file_exists($setting_file)) {
            $simpleid_settings[$name] = unserialize(file_get_contents($setting_file));
        } else {
            return $default;
        }
    }
    
    return $simpleid_settings[$name];
}

/**
 * Saves an application setting.
 *
 * @param string $name the name of the setting to save
 * @param mixed $value the value of the setting
 *
 */
function store_set($name, $value) {
    global $simpleid_settings;
    
    if (!_store_is_valid_name($name)) {
        trigger_error("Invalid setting name for filesystem store", E_USER_ERROR);
        return;
    }
    
    $simpleid_settings[$name] = $value;
    
    $setting_file = SIMPLEID_STORE_DIR . "/$name.setting";
    $file = fopen($setting_file, 'w');
    fwrite($file, serialize($value));
    fclose($file);
}

/**
 * Deletes an application setting.
 *
 * @param string $name the name of the setting to delete
 *
 */
function store_del($name) {
    global $simpleid_settings;
    
    if (!_store_is_valid_name($name)) {
        trigger_error("Invalid setting name for filesystem store", E_USER_ERROR);
        return;
    }
    
    if (isset($simpleid_settings[$name])) unset($simpleid_settings[$name]);
    
    $setting_file = SIMPLEID_STORE_DIR . "/$name.setting";
    if (file_exists($setting_file)) unlink($setting_file);
}

/**
 * Determines whether a name is a valid name for use with this store.
 *
 * For file system storage, a name is not valid if it contains either a
 * directory separator (i.e. / or \).
 *
 * @param string $name the name to check
 * @return boolean whether the name is valid for use with this store 
 *
 */
function _store_is_valid_name($name) {
    return preg_match('!\A[^/\\\\]*\z!', $name);
}
?>