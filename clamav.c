/*
 *  Copyright (C) 2009 Argos <argos66@gmail.com>
 *  Copyright (C) 2005 Geffrey Velasquez Torres <geffrey@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include "ext/standard/info.h"
#include "php_clamav.h"
#include <string.h>
#include <stdio.h>
#include <clamav.h>

ZEND_DECLARE_MODULE_GLOBALS(clamav)

/* True global resources - no need for thread safety here */
static int le_clamav;
static struct cl_engine *dbengine;  /* internal db engine */
static unsigned int sig_num = 0;    /* signature number */
struct cl_stat dbstat;              /* database stat */

#ifdef ZEND_ENGINE_2
static
	ZEND_BEGIN_ARG_INFO(second_args_force_ref, 0)
		ZEND_ARG_PASS_INFO(0)
		ZEND_ARG_PASS_INFO(1)
	ZEND_END_ARG_INFO()
#else /* ZEND_ENGINE_1 */
static unsigned char second_args_force_ref[] = 
    { 2, BYREF_NONE, BYREF_FORCE };
#endif

/* {{{ clamav_functions[]
 *
 * Every user visible function must have an entry in clamav_functions[].
 */
function_entry clamav_functions[] = {
    PHP_FE(cl_info, NULL)
    PHP_FE(cl_scanfile, second_args_force_ref)
    PHP_FE(cl_engine, NULL)
    PHP_FE(cl_pretcode, NULL)
    PHP_FE(cl_version, NULL)
    PHP_FE(cl_debug, NULL)
    {NULL, NULL, NULL}       /* Must be the last line in clamav_functions[] */
};
/* }}} */

/* {{{ clamav_module_entry
 */
zend_module_entry clamav_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
    "clamav",
    clamav_functions,
    PHP_MINIT(clamav),
    PHP_MSHUTDOWN(clamav),
    PHP_RINIT(clamav),		
    PHP_RSHUTDOWN(clamav),
    PHP_MINFO(clamav),
#if ZEND_MODULE_API_NO >= 20010901
	PHP_CLAMAV_VERSION,
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_CLAMAV
ZEND_GET_MODULE(clamav)
#endif

/* {{{ PHP_INI
 */
PHP_INI_BEGIN()

    STD_PHP_INI_ENTRY("clamav.dbpath", "/var/lib/clamav", PHP_INI_ALL, 
					  OnUpdateString, dbpath, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.maxreclevel", "16", PHP_INI_ALL, OnUpdateLong, 
				 	  maxreclevel, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.maxfiles", "10000", PHP_INI_ALL, OnUpdateLong, 
					  maxfiles, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.maxfilesize", "26214400", PHP_INI_ALL, OnUpdateLong, 
					  maxfilesize, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.maxscansize", "104857600", PHP_INI_ALL, OnUpdateLong, 
					  maxscansize, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.keeptmp", "0", PHP_INI_ALL, OnUpdateLong, 
					  keeptmp, zend_clamav_globals, clamav_globals)

    STD_PHP_INI_ENTRY("clamav.tmpdir", "/tmp", PHP_INI_ALL, OnUpdateString, 
					  tmpdir, zend_clamav_globals, clamav_globals)

PHP_INI_END()
/* }}} */

/* {{{ php_clamav_init_globals
 */
static void php_clamav_init_globals(zend_clamav_globals *clamav_globals)
{
    clamav_globals->dbpath        = NULL;
    clamav_globals->maxreclevel   = 0;
    clamav_globals->maxfiles      = 0;
    clamav_globals->maxfilesize   = 0;
    clamav_globals->maxscansize   = 0;
    clamav_globals->keeptmp       = 0;
    clamav_globals->tmpdir        = NULL;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(clamav)
{
	ZEND_INIT_MODULE_GLOBALS(clamav, php_clamav_init_globals, NULL);
	REGISTER_INI_ENTRIES();

	int ret;	/* return value */

	/* Initialization */
	ret = cl_init(CL_INIT_DEFAULT);

	if (ret != CL_SUCCESS) {
		php_error(E_WARNING, "cl_init: result %i failed (%s)\n", ret, cl_strerror(ret));
		return FAILURE;
	}

	if (!(dbengine = cl_engine_new())){
		php_error(E_WARNING, "Can’t create new engine\n");
		return FAILURE;
	}

	/* database loading */
	if ((ret = cl_load(cl_retdbdir(), dbengine, &sig_num, CL_DB_STDOPT))) {
		php_error(E_WARNING, "cl_load: failed (%s)\n", cl_strerror(ret));
		return FAILURE;
	}

	if ((ret = cl_engine_compile(dbengine)) != CL_SUCCESS) {
        php_error(E_WARNING, "cl_engine_compile() error: %s\n", cl_strerror(ret));
        cl_engine_free(dbengine);
        return FAILURE;
	}

	/* database stat */
    memset(&dbstat, 0, sizeof(struct cl_stat));
    cl_statinidir(CLAMAV_G(dbpath), &dbstat);

	/* engine parameters */
	cl_engine_set_num(dbengine, CL_ENGINE_MAX_FILES, CLAMAV_G(maxfiles));
	cl_engine_set_num(dbengine, CL_ENGINE_MAX_FILESIZE, CLAMAV_G(maxfilesize));
    cl_engine_set_num(dbengine, CL_ENGINE_MAX_SCANSIZE, CLAMAV_G(maxscansize));
	cl_engine_set_num(dbengine, CL_ENGINE_MAX_RECURSION, CLAMAV_G(maxreclevel));
	cl_engine_set_num(dbengine, CL_ENGINE_KEEPTMP, CLAMAV_G(keeptmp));
	cl_engine_set_str(dbengine, CL_ENGINE_TMPDIR, CLAMAV_G(tmpdir));

	/*  Register ClamAV scan options, they're also availible inside
	 *  php scripts with the same name and value.
	 */
	REGISTER_LONG_CONSTANT("CL_SCAN_RAW", CL_SCAN_RAW, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_ARCHIVE", CL_SCAN_ARCHIVE, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_MAIL", CL_SCAN_MAIL, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_OLE2", CL_SCAN_OLE2, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_BLOCKENCRYPTED", CL_SCAN_BLOCKENCRYPTED, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_HTML", CL_SCAN_HTML, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_PE", CL_SCAN_PE, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_BLOCKBROKEN", CL_SCAN_BLOCKBROKEN, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_MAILURL", CL_SCAN_MAILURL, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_BLOCKMAX", CL_SCAN_BLOCKMAX, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_ALGORITHMIC", CL_SCAN_ALGORITHMIC, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_ELF", CL_SCAN_ELF, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_PDF", CL_SCAN_PDF, 
							CONST_CS | CONST_PERSISTENT);
	REGISTER_LONG_CONSTANT("CL_SCAN_STDOPT", CL_SCAN_STDOPT, 
							CONST_CS | CONST_PERSISTENT);

	/*  Register ClamAV return codes, they're also available
	 *  inside php scripts with the same name and value.
	 */
    REGISTER_LONG_CONSTANT("CL_CLEAN", CL_CLEAN,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_VIRUS", CL_VIRUS,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ENULLARG", CL_ENULLARG,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EARG", CL_EARG,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMALFDB", CL_EMALFDB,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ECVD", CL_ECVD,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EVERIFY", CL_EVERIFY,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EUNPACK", CL_EUNPACK,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EOPEN", CL_EOPEN,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ECREAT", CL_ECREAT,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EUNLINK", CL_EUNLINK,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ESTAT", CL_ESTAT,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EREAD", CL_EREAD,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ESEEK", CL_ESEEK,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EWRITE", CL_EWRITE,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EDUP", CL_EDUP,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EACCES", CL_EACCES,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ETMPFILE", CL_ETMPFILE,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ETMPDIR", CL_ETMPDIR,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMAP", CL_EMAP,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMEM", CL_EMEM,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_ETIMEOUT", CL_ETIMEOUT,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMAXREC", CL_EMAXREC,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMAXSIZE", CL_EMAXSIZE,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EMAXFILES", CL_EMAXFILES,
                            CONST_CS | CONST_PERSISTENT);
    REGISTER_LONG_CONSTANT("CL_EFORMAT", CL_EFORMAT,
                            CONST_CS | CONST_PERSISTENT);

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(clamav)
{
	UNREGISTER_INI_ENTRIES();

	/* free data structures*/    
	cl_statfree(&dbstat);

	if (dbengine) {	
		cl_engine_free(dbengine);
	}

	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request start */
/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(clamav)
{
    int ret;              /* return value */

    if (cl_statchkdir(&dbstat) == 1) {
		  /* database reload */
		if (dbengine) {
		 	cl_engine_free(dbengine);
		}

		dbengine = NULL;
		sig_num  = 0;

		if (!(dbengine = cl_engine_new())){
			php_error(E_WARNING, "Can’t create new engine\n");
			return FAILURE;
		}

		if ((ret = cl_load(cl_retdbdir(), dbengine, &sig_num, CL_DB_STDOPT))) {
			php_error(E_WARNING, "cl_load: failed (%s)\n", cl_strerror(ret));
			return FAILURE;
		}

		if((ret = cl_engine_compile(dbengine)) != CL_SUCCESS) {
            php_error(E_WARNING, "cl_engine_compile() error: %s\n", cl_strerror(ret));
            cl_engine_free(dbengine);
            return FAILURE;
		}

		cl_statfree(&dbstat);
    	cl_statinidir(cl_retdbdir(), &dbstat);
    }

	return SUCCESS;
}
/* }}} */

/* Remove if there's nothing to do at request end */
/* {{{ PHP_RSHUTDOWN_FUNCTION
 */
PHP_RSHUTDOWN_FUNCTION(clamav)
{
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(clamav)
{
    php_info_print_table_start();
    php_info_print_table_row(2, "Clamav support", "enabled");
    php_info_print_table_row(2, "php-clamav version", PHP_CLAMAV_VERSION);
    php_info_print_table_row(2, "libclamav version", cl_retver());
//    php_info_print_table_row(2, "number clamav signatures", Z_LVAL_P(sig_num));
//TODO: Add number of signature to php_info()
    php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/* {{{ proto void cl_info()
    Prints ClamAV informations */
PHP_FUNCTION(cl_info)
{
    php_printf("ClamAV version %s with %d virus signatures loaded", cl_retver(), sig_num);
}
/* }}} */

/* {{{ proto void cl_version()
    Prints only ClamAV version */
PHP_FUNCTION(cl_version)
{
    const char *clam_v = cl_retver();
    RETURN_STRINGL((char *)clam_v,strlen(clam_v),1);
}
/* }}} */

/* {{{ proto int cl_scanfile(string filename) 
 * Scans the contents of a file, given a filename, returns the virus name
 * (if it was found) and return ClamAV cl_scanfile result code */
PHP_FUNCTION(cl_scanfile)
{
    /* number of arguments */
	const int NUM_ARGS = 2;

    /* parameters */
    zval *filename;   	/* file to be scanned */
    zval *virusname;    /* for return virus name if found */

	int ret;			/* clamav functions return value */
	const char *virname	= NULL;

	/* argument checking */
	if (ZEND_NUM_ARGS() != NUM_ARGS) {
		WRONG_PARAM_COUNT;
		RETURN_FALSE;
	}

	/* argument parsing */
	if (zend_parse_parameters(NUM_ARGS TSRMLS_CC, "zz", &filename, &virusname) != SUCCESS) {
        WRONG_PARAM_COUNT;
		RETURN_FALSE;
	}

	/* parameter conversion */
	convert_to_string_ex(&filename);

	/* clean up old values first */
	zval_dtor(virusname);

	/* executing the ClamAV virus checking function */
    ret = cl_scanfile(Z_STRVAL_P(filename), &virname, 0, dbengine, CL_SCAN_STDOPT);

	/* copy the value of the cl_scanfile virusname if a virus was found */
	if (ret == CL_VIRUS) {
        ZVAL_STRING(virusname, virname, 1);
    } else if (ret != CL_CLEAN) {
        php_error(E_WARNING,"Error: %s", cl_strerror(ret));
	}

    RETURN_LONG(ret);
}
/* }}} */

/* {{{ proto void cl_engine()
    Set the ClamAV parameters for scanning */ 
PHP_FUNCTION(cl_engine)
{
	/* number of arguments */
	const int NUM_ARGS = 5;

	long long maxfiles;
	long long maxfilesize;
    long long maxscansize;
	long      maxreclevel;
	long      keeptmp;

	/* argument checking */
	if (ZEND_NUM_ARGS() != NUM_ARGS) {
		WRONG_PARAM_COUNT;
		RETURN_FALSE;
		return;
	}

	/* argument parsing */
	if (zend_parse_parameters(NUM_ARGS TSRMLS_CC, "lllll", 
							  &maxfiles, &maxfilesize, &maxscansize,
							  &maxreclevel, &keeptmp ) != SUCCESS) {
		RETURN_FALSE;
	}

	/* set engine parameters */
    cl_engine_set_num(dbengine, CL_ENGINE_MAX_FILES, maxfiles);
    cl_engine_set_num(dbengine, CL_ENGINE_MAX_FILESIZE, maxfilesize);
    cl_engine_set_num(dbengine, CL_ENGINE_MAX_SCANSIZE, maxscansize);
    cl_engine_set_num(dbengine, CL_ENGINE_MAX_RECURSION, maxreclevel);
    cl_engine_set_num(dbengine, CL_ENGINE_KEEPTMP, keeptmp);
}
/* }}} */

/* {{{ proto string cl_pretcode(int retcode)
    Translates the ClamAV return code */
PHP_FUNCTION(cl_pretcode)
{
	/* number of arguments */
	const int NUM_ARGS	= 1;

	/* parameters */
    long retcode;

	if (ZEND_NUM_ARGS() != NUM_ARGS) {
		WRONG_PARAM_COUNT;
    }

	/* parameters parsing */
	if (zend_parse_parameters(NUM_ARGS TSRMLS_CC, "l", &retcode) != SUCCESS) {
		WRONG_PARAM_COUNT;
    }

	switch (retcode) {
        /* libclamav specific errors*/
		case CL_CLEAN:
			RETURN_STRING("virus not found", 1);
			break;
		case CL_VIRUS:
			RETURN_STRING("virus found", 1);
			break;
		case CL_ENULLARG:
			RETURN_STRING("null argument error", 1);
			break;
		case CL_EARG:
			RETURN_STRING("argument error", 1);
			break;
		case CL_EMALFDB:
			RETURN_STRING("malformed database", 1);
			break;
		case CL_ECVD:
			RETURN_STRING("CVD error", 1);
			break;
		case CL_EVERIFY:
			RETURN_STRING("verification error", 1);
			break;
		case CL_EUNPACK:
			RETURN_STRING("uncompression error", 1);
			break;

        /* I/O and memory errors */
		case CL_EOPEN:
			RETURN_STRING("CL_EOPEN error", 1);
			break;
		case CL_ECREAT:
			RETURN_STRING("CL_ECREAT error", 1);
			break;
		case CL_EUNLINK:
			RETURN_STRING("CL_EUNLINK error", 1);
			break;
		case CL_ESTAT:
			RETURN_STRING("CL_ESTAT error", 1);
			break;
		case CL_EREAD:
			RETURN_STRING("CL_EREAD error", 1);
			break;
		case CL_ESEEK:
			RETURN_STRING("CL_ESEEK error", 1);
			break;
		case CL_EWRITE:
			RETURN_STRING("CL_EWRITE error", 1);
			break;
		case CL_EDUP:
			RETURN_STRING("CL_EDUP error", 1);
			break;
		case CL_EACCES:
			RETURN_STRING("CL_EACCES error", 1);
			break;
		case CL_ETMPFILE:
			RETURN_STRING("CL_ETMPFILE error", 1);
			break;
        case CL_ETMPDIR:
			RETURN_STRING("CL_ETMPDIR error", 1);
			break;
		case CL_EMAP:
			RETURN_STRING("CL_EMAP error", 1);
			break;
		case CL_EMEM:
			RETURN_STRING("CL_EMEM error", 1);
			break;
		case CL_ETIMEOUT:
			RETURN_STRING("CL_ETIMEOUT error", 1);
			break;

        /* internal (not reported outside libclamav) */
		case CL_BREAK:
			RETURN_STRING("CL_BREAK error", 1);
			break;
		case CL_EMAXREC:
			RETURN_STRING("recursion level limit exceeded", 1);
			break;
		case CL_EMAXSIZE:
			RETURN_STRING("maximum file size limit exceeded", 1);
			break;
		case CL_EMAXFILES:
			RETURN_STRING("maximum files limit exceeded", 1);
			break;
		case CL_EFORMAT:
			RETURN_STRING("bad format or broken file", 1);
			break;

		default :
			RETURN_STRING("unknow return code", 1);
			break;
	}

	return;
}
/* }}} */

/* {{{ proto string cl_debug()
    Turn on ClamAV debug mode */
PHP_FUNCTION(cl_debug)
{
	cl_debug();
}
/* }}} */
