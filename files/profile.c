/*
 * Profile functions
 *
 * Copyright 1993 Miguel de Icaza
 * Copyright 1996 Alexandre Julliard
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>

#include "windef.h"
#include "winbase.h"
#include "winnls.h"
#include "winerror.h"
#include "wine/winbase16.h"
#include "winreg.h"
#include "file.h"
#include "heap.h"
#include "debugtools.h"
#include "options.h"
#include "wine/server.h"

DEFAULT_DEBUG_CHANNEL(profile);

typedef struct tagPROFILEKEY
{
    char                  *value;
    struct tagPROFILEKEY  *next;
    char                   name[1];
} PROFILEKEY;

typedef struct tagPROFILESECTION
{
    struct tagPROFILEKEY       *key;
    struct tagPROFILESECTION   *next;
    char                        name[1];
} PROFILESECTION;


typedef struct
{
    BOOL             changed;
    PROFILESECTION  *section;
    char            *dos_name;
    char            *unix_name;
    char            *filename;
    time_t           mtime;
} PROFILE;


#define N_CACHED_PROFILES 10

/* Cached profile files */
static PROFILE *MRUProfile[N_CACHED_PROFILES]={NULL};

#define CurProfile (MRUProfile[0])

/* wine.ini config file registry root */
static HKEY wine_profile_key;

#define PROFILE_MAX_LINE_LEN   1024

/* Wine profile name in $HOME directory; must begin with slash */
static const char PROFILE_WineIniName[] = "/.winerc";

/* Wine profile: the profile file being used */
static char PROFILE_WineIniUsed[MAX_PATHNAME_LEN] = "";

/* Check for comments in profile */
#define IS_ENTRY_COMMENT(str)  ((str)[0] == ';')

static const WCHAR wininiW[] = { 'w','i','n','.','i','n','i',0 };

static CRITICAL_SECTION PROFILE_CritSect = CRITICAL_SECTION_INIT;

static const char hex[16] = "0123456789ABCDEF";

/***********************************************************************
 *           PROFILE_CopyEntry
 *
 * Copy the content of an entry into a buffer, removing quotes, and possibly
 * translating environment variables.
 */
static void PROFILE_CopyEntry( char *buffer, const char *value, int len,
                               int handle_env )
{
    char quote = '\0';
    const char *p;

    if(!buffer) return;

    if ((*value == '\'') || (*value == '\"'))
    {
        if (value[1] && (value[strlen(value)-1] == *value)) quote = *value++;
    }

    if (!handle_env)
    {
        lstrcpynA( buffer, value, len );
        if (quote && (len >= strlen(value))) buffer[strlen(buffer)-1] = '\0';
        return;
    }

    for (p = value; (*p && (len > 1)); *buffer++ = *p++, len-- )
    {
        if ((*p == '$') && (p[1] == '{'))
        {
            char env_val[1024];
            const char *env_p;
            const char *p2 = strchr( p, '}' );
            if (!p2) continue;  /* ignore it */
            lstrcpynA(env_val, p + 2, min( sizeof(env_val), (int)(p2-p)-1 ));
            if ((env_p = getenv( env_val )) != NULL)
            {
                int buffer_len;
                lstrcpynA( buffer, env_p, len );
                buffer_len = strlen( buffer );
                buffer += buffer_len;
                len -= buffer_len;
            }
            p = p2 + 1;
        }
    }
    if (quote && (len > 1)) buffer--;
    *buffer = '\0';
}


/***********************************************************************
 *           PROFILE_Save
 *
 * Save a profile tree to a file.
 */
static void PROFILE_Save( FILE *file, PROFILESECTION *section )
{
    PROFILEKEY *key;

    for ( ; section; section = section->next)
    {
        if (section->name) fprintf( file, "\r\n[%s]\r\n", section->name );
        for (key = section->key; key; key = key->next)
        {
            fprintf( file, "%s", key->name );
            if (key->value) fprintf( file, "=%s", key->value );
            fprintf( file, "\r\n" );
        }
    }
}


/***********************************************************************
 *           PROFILE_Free
 *
 * Free a profile tree.
 */
static void PROFILE_Free( PROFILESECTION *section )
{
    PROFILESECTION *next_section;
    PROFILEKEY *key, *next_key;

    for ( ; section; section = next_section)
    {
        for (key = section->key; key; key = next_key)
        {
            next_key = key->next;
            if (key->value) HeapFree( GetProcessHeap(), 0, key->value );
            HeapFree( GetProcessHeap(), 0, key );
        }
        next_section = section->next;
        HeapFree( GetProcessHeap(), 0, section );
    }
}

static inline int PROFILE_isspace(char c)
{
	if (isspace(c)) return 1;
	if (c=='\r' || c==0x1a) return 1;
	/* CR and ^Z (DOS EOF) are spaces too  (found on CD-ROMs) */
	return 0;
}


/***********************************************************************
 *           PROFILE_Load
 *
 * Load a profile tree from a file.
 */
static PROFILESECTION *PROFILE_Load( FILE *file )
{
    char buffer[PROFILE_MAX_LINE_LEN];
    char *p, *p2;
    int line = 0;
    PROFILESECTION *section, *first_section;
    PROFILESECTION **next_section;
    PROFILEKEY *key, *prev_key, **next_key;

    first_section = HeapAlloc( GetProcessHeap(), 0, sizeof(*section) );
    if(first_section == NULL) return NULL;
    first_section->name[0] = 0;
    first_section->key  = NULL;
    first_section->next = NULL;
    next_section = &first_section->next;
    next_key     = &first_section->key;
    prev_key     = NULL;

    while (fgets( buffer, PROFILE_MAX_LINE_LEN, file ))
    {
        line++;
        p = buffer;
        while (*p && PROFILE_isspace(*p)) p++;
        if (*p == '[')  /* section start */
        {
            if (!(p2 = strrchr( p, ']' )))
            {
                WARN("Invalid section header at line %d: '%s'\n",
		     line, p );
            }
            else
            {
                *p2 = '\0';
                p++;
                if (!(section = HeapAlloc( GetProcessHeap(), 0, sizeof(*section) + strlen(p) )))
                    break;
                strcpy( section->name, p );
                section->key  = NULL;
                section->next = NULL;
                *next_section = section;
                next_section  = &section->next;
                next_key      = &section->key;
                prev_key      = NULL;

                TRACE("New section: '%s'\n",section->name);

                continue;
            }
        }

        p2=p+strlen(p) - 1;
        while ((p2 > p) && ((*p2 == '\n') || PROFILE_isspace(*p2))) *p2--='\0';

        if ((p2 = strchr( p, '=' )) != NULL)
        {
            char *p3 = p2 - 1;
            while ((p3 > p) && PROFILE_isspace(*p3)) *p3-- = '\0';
            *p2++ = '\0';
            while (*p2 && PROFILE_isspace(*p2)) p2++;
        }

        if(*p || !prev_key || *prev_key->name)
        {
            if (!(key = HeapAlloc( GetProcessHeap(), 0, sizeof(*key) + strlen(p) ))) break;
            strcpy( key->name, p );
            if (p2)
            {
                key->value = HeapAlloc( GetProcessHeap(), 0, strlen(p2)+1 );
                strcpy( key->value, p2 );
            }
            else key->value = NULL;

           key->next  = NULL;
           *next_key  = key;
           next_key   = &key->next;
           prev_key   = key;

           TRACE("New key: name='%s', value='%s'\n",key->name,key->value?key->value:"(none)");
        }
    }
    return first_section;
}

/* convert the .winerc file to the new format */
static void convert_config( FILE *in, const char *output_name )
{
    char buffer[PROFILE_MAX_LINE_LEN];
    char *p, *p2;
    FILE *out;

    /* create the output file, only if it doesn't exist already */
    int fd = open( output_name, O_WRONLY|O_CREAT|O_EXCL, 0666 );
    if (fd == -1)
    {
        MESSAGE( "Could not create new config file '%s': %s\n", output_name, strerror(errno) );
        ExitProcess(1);
    }

    out = fdopen( fd, "w" );
    fprintf( out, "WINE REGISTRY Version 2\n" );
    fprintf( out, ";; All keys relative to \\\\Machine\\\\Software\\\\Wine\\\\Wine\\\\Config\n\n" );
    while (fgets( buffer, PROFILE_MAX_LINE_LEN, in ))
    {
        if (buffer[strlen(buffer)-1] == '\n') buffer[strlen(buffer)-1] = 0;
        p = buffer;
        while (*p && PROFILE_isspace(*p)) p++;
        if (*p == '[')  /* section start */
        {
            if ((p2 = strrchr( p, ']' )))
            {
                *p2 = '\0';
                p++;
                fprintf( out, "[%s]\n", p );
            }
            continue;
        }

        if (*p == ';' || *p == '#')
        {
            fprintf( out, "%s\n", p );
            continue;
        }

        p2=p+strlen(p) - 1;
        while ((p2 > p) && ((*p2 == '\n') || PROFILE_isspace(*p2))) *p2--='\0';

        if ((p2 = strchr( p, '=' )) != NULL)
        {
            char *p3 = p2 - 1;
            while ((p3 > p) && PROFILE_isspace(*p3)) *p3-- = '\0';
            *p2++ = '\0';
            while (*p2 && PROFILE_isspace(*p2)) p2++;
        }

        if (!*p)
        {
            fprintf( out, "\n" );
            continue;
        }
        fputc( '"', out );
        while (*p)
        {
            if (*p == '\\') fputc( '\\', out );
            fputc( *p, out );
            p++;
        }
        fprintf( out, "\" = \"" );
        if (p2)
        {
            while (*p2)
            {
                if (*p2 == '\\') fputc( '\\', out );
                fputc( *p2, out );
                p2++;
            }
        }
        fprintf( out, "\"\n" );
    }
    fclose( out );
}


/***********************************************************************
 *           PROFILE_DeleteSection
 *
 * Delete a section from a profile tree.
 */
static BOOL PROFILE_DeleteSection( PROFILESECTION **section, LPCSTR name )
{
    while (*section)
    {
        if ((*section)->name[0] && !strcasecmp( (*section)->name, name ))
        {
            PROFILESECTION *to_del = *section;
            *section = to_del->next;
            to_del->next = NULL;
            PROFILE_Free( to_del );
            return TRUE;
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_DeleteKey
 *
 * Delete a key from a profile tree.
 */
static BOOL PROFILE_DeleteKey( PROFILESECTION **section,
			       LPCSTR section_name, LPCSTR key_name )
{
    while (*section)
    {
        if ((*section)->name[0] && !strcasecmp( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                if (!strcasecmp( (*key)->name, key_name ))
                {
                    PROFILEKEY *to_del = *key;
                    *key = to_del->next;
                    if (to_del->value) HeapFree( GetProcessHeap(), 0, to_del->value);
                    HeapFree( GetProcessHeap(), 0, to_del );
                    return TRUE;
                }
                key = &(*key)->next;
            }
        }
        section = &(*section)->next;
    }
    return FALSE;
}


/***********************************************************************
 *           PROFILE_DeleteAllKeys
 *
 * Delete all keys from a profile tree.
 */
void PROFILE_DeleteAllKeys( LPCSTR section_name)
{
    PROFILESECTION **section= &CurProfile->section;
    while (*section)
    {
        if ((*section)->name[0] && !strcasecmp( (*section)->name, section_name ))
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                PROFILEKEY *to_del = *key;
		*key = to_del->next;
		if (to_del->value) HeapFree( GetProcessHeap(), 0, to_del->value);
		HeapFree( GetProcessHeap(), 0, to_del );
		CurProfile->changed =TRUE;
            }
        }
        section = &(*section)->next;
    }
}


/***********************************************************************
 *           PROFILE_Find
 *
 * Find a key in a profile tree, optionally creating it.
 */
static PROFILEKEY *PROFILE_Find( PROFILESECTION **section,
                                 const char *section_name,
                                 const char *key_name, int create )
{
    const char *p;
    int seclen, keylen;

    while (PROFILE_isspace(*section_name)) section_name++;
    p = section_name + strlen(section_name) - 1;
    while ((p > section_name) && PROFILE_isspace(*p)) p--;
    seclen = p - section_name + 1;
    
    while (PROFILE_isspace(*key_name)) key_name++;
    p = key_name + strlen(key_name) - 1;
    while ((p > key_name) && PROFILE_isspace(*p)) p--;
    keylen = p - key_name + 1;

    while (*section)
    {
        if ( ((*section)->name[0])
	  && (!(strncasecmp( (*section)->name, section_name, seclen )))
	  && (((*section)->name)[seclen] == '\0') )
        {
            PROFILEKEY **key = &(*section)->key;
            while (*key)
            {
                if ( (!(strncasecmp( (*key)->name, key_name, keylen )))
		  && (((*key)->name)[keylen] == '\0') )
		    return *key;
                key = &(*key)->next;
            }
            if (!create) return NULL;
            if (!(*key = HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILEKEY) + strlen(key_name) )))
                return NULL;
            strcpy( (*key)->name, key_name );
            (*key)->value = NULL;
            (*key)->next  = NULL;
            return *key;
        }
        section = &(*section)->next;
    }
    if (!create) return NULL;
    *section = HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILESECTION) + strlen(section_name) );
    if(*section == NULL) return NULL;
    strcpy( (*section)->name, section_name );
    (*section)->next = NULL;
    if (!((*section)->key  = HeapAlloc( GetProcessHeap(), 0,
                                        sizeof(PROFILEKEY) + strlen(key_name) )))
    {
        HeapFree(GetProcessHeap(), 0, *section);
        return NULL;
    }
    strcpy( (*section)->key->name, key_name );
    (*section)->key->value = NULL;
    (*section)->key->next  = NULL;
    return (*section)->key;
}


/***********************************************************************
 *           PROFILE_FlushFile
 *
 * Flush the current profile to disk if changed.
 */
static BOOL PROFILE_FlushFile(void)
{
    char *p, buffer[MAX_PATHNAME_LEN];
    const char *unix_name;
    FILE *file = NULL;
    struct stat buf;

    if(!CurProfile)
    {
        WARN("No current profile!\n");
        return FALSE;
    }

    if (!CurProfile->changed || !CurProfile->dos_name) return TRUE;
    if (!(unix_name = CurProfile->unix_name) || !(file = fopen(unix_name, "w")))
    {
        /* Try to create it in $HOME/.wine */
        /* FIXME: this will need a more general solution */
        strcpy( buffer, get_config_dir() );
        p = buffer + strlen(buffer);
        *p++ = '/';
        strcpy( p, strrchr( CurProfile->dos_name, '\\' ) + 1 );
        _strlwr( p );
        file = fopen( buffer, "w" );
        unix_name = buffer;
    }
    
    if (!file)
    {
        WARN("could not save profile file %s\n", CurProfile->dos_name);
        return FALSE;
    }

    TRACE("Saving '%s' into '%s'\n", CurProfile->dos_name, unix_name );
    PROFILE_Save( file, CurProfile->section );
    fclose( file );
    CurProfile->changed = FALSE;
    if(!stat(unix_name,&buf))
       CurProfile->mtime=buf.st_mtime;
    return TRUE;
}


/***********************************************************************
 *           PROFILE_ReleaseFile
 *
 * Flush the current profile to disk and remove it from the cache.
 */
static void PROFILE_ReleaseFile(void)
{
    PROFILE_FlushFile();
    PROFILE_Free( CurProfile->section );
    if (CurProfile->dos_name) HeapFree( GetProcessHeap(), 0, CurProfile->dos_name );
    if (CurProfile->unix_name) HeapFree( GetProcessHeap(), 0, CurProfile->unix_name );
    if (CurProfile->filename) HeapFree( GetProcessHeap(), 0, CurProfile->filename );
    CurProfile->changed   = FALSE;
    CurProfile->section   = NULL;
    CurProfile->dos_name  = NULL;
    CurProfile->unix_name = NULL;
    CurProfile->filename  = NULL;
    CurProfile->mtime     = 0;
}


/***********************************************************************
 *           PROFILE_Open
 *
 * Open a profile file, checking the cached file first.
 */
static BOOL PROFILE_Open( LPCSTR filename )
{
    DOS_FULL_NAME full_name;
    char buffer[MAX_PATHNAME_LEN];
    char *newdos_name, *p;
    FILE *file = NULL;
    int i,j;
    struct stat buf;
    PROFILE *tempProfile;

    /* First time around */

    if(!CurProfile)
       for(i=0;i<N_CACHED_PROFILES;i++)
         {
          MRUProfile[i]=HeapAlloc( GetProcessHeap(), 0, sizeof(PROFILE) );
	  if(MRUProfile[i] == NULL) break;
          MRUProfile[i]->changed=FALSE;
          MRUProfile[i]->section=NULL;
          MRUProfile[i]->dos_name=NULL;
          MRUProfile[i]->unix_name=NULL;
          MRUProfile[i]->filename=NULL;
          MRUProfile[i]->mtime=0;
         }

    /* Check for a match */

    if (strchr( filename, '/' ) || strchr( filename, '\\' ) || 
        strchr( filename, ':' ))
    {
        if (!DOSFS_GetFullName( filename, FALSE, &full_name )) return FALSE;
    }
    else
    {
        GetWindowsDirectoryA( buffer, sizeof(buffer) );
        strcat( buffer, "\\" );
        strcat( buffer, filename );
        if (!DOSFS_GetFullName( buffer, FALSE, &full_name )) return FALSE;
    }

    for(i=0;i<N_CACHED_PROFILES;i++)
      {
       if ((MRUProfile[i]->filename && !strcmp( filename, MRUProfile[i]->filename )) ||
           (MRUProfile[i]->dos_name && !strcmp( full_name.short_name, MRUProfile[i]->dos_name )))
         {
          if(i)
            {
             PROFILE_FlushFile();
             tempProfile=MRUProfile[i];
             for(j=i;j>0;j--)
                MRUProfile[j]=MRUProfile[j-1];
             CurProfile=tempProfile;
            }
          if(!stat(CurProfile->unix_name,&buf) && CurProfile->mtime==buf.st_mtime)
             TRACE("(%s): already opened (mru=%d)\n",
                              filename, i );
          else
              TRACE("(%s): already opened, needs refreshing (mru=%d)\n",
                             filename, i );
	  return TRUE;
         }
      }

    /* Flush the old current profile */
    PROFILE_FlushFile();

    /* Make the oldest profile the current one only in order to get rid of it */
    if(i==N_CACHED_PROFILES)
      {
       tempProfile=MRUProfile[N_CACHED_PROFILES-1];
       for(i=N_CACHED_PROFILES-1;i>0;i--)
          MRUProfile[i]=MRUProfile[i-1];
       CurProfile=tempProfile;
      }
    if(CurProfile->filename) PROFILE_ReleaseFile();

    /* OK, now that CurProfile is definitely free we assign it our new file */
    newdos_name = HeapAlloc( GetProcessHeap(), 0, strlen(full_name.short_name)+1 );
    strcpy( newdos_name, full_name.short_name );
    CurProfile->dos_name  = newdos_name;
    CurProfile->filename  = HeapAlloc( GetProcessHeap(), 0, strlen(filename)+1 );
    strcpy( CurProfile->filename, filename );

    /* Try to open the profile file, first in $HOME/.wine */

    /* FIXME: this will need a more general solution */
    strcpy( buffer, get_config_dir() );
    p = buffer + strlen(buffer);
    *p++ = '/';
    strcpy( p, strrchr( newdos_name, '\\' ) + 1 );
    _strlwr( p );
    if ((file = fopen( buffer, "r" )))
    {
        TRACE("(%s): found it in %s\n",
              filename, buffer );
        CurProfile->unix_name = HeapAlloc( GetProcessHeap(), 0, strlen(buffer)+1 );
        strcpy( CurProfile->unix_name, buffer );
    }

    if (!file)
    {
        CurProfile->unix_name = HeapAlloc( GetProcessHeap(), 0, strlen(full_name.long_name)+1 );
        strcpy( CurProfile->unix_name, full_name.long_name );
        if ((file = fopen( full_name.long_name, "r" )))
            TRACE("(%s): found it in %s\n",
                             filename, full_name.long_name );
    }

    if (file)
    {
        CurProfile->section = PROFILE_Load( file );
        fclose( file );
        if(!stat(CurProfile->unix_name,&buf))
           CurProfile->mtime=buf.st_mtime;
    }
    else
    {
        /* Does not exist yet, we will create it in PROFILE_FlushFile */
        WARN("profile file %s not found\n", newdos_name );
    }
    return TRUE;
}


/***********************************************************************
 *           PROFILE_GetSection
 *
 * Returns all keys of a section.
 * If return_values is TRUE, also include the corresponding values.
 */
static INT PROFILE_GetSection( PROFILESECTION *section, LPCSTR section_name,
			       LPSTR buffer, UINT len, BOOL handle_env,
			       BOOL return_values )
{
    PROFILEKEY *key;

    if(!buffer) return 0;

    while (section)
    {
        if (section->name[0] && !strcasecmp( section->name, section_name ))
        {
            UINT oldlen = len;
            for (key = section->key; key; key = key->next)
            {
                if (len <= 2) break;
                if (!*key->name) continue;  /* Skip empty lines */
                if (IS_ENTRY_COMMENT(key->name)) continue;  /* Skip comments */
                PROFILE_CopyEntry( buffer, key->name, len - 1, handle_env );
                len -= strlen(buffer) + 1;
                buffer += strlen(buffer) + 1;
		if (len < 2)
		    break;
		if (return_values && key->value) {
			buffer[-1] = '=';
			PROFILE_CopyEntry ( buffer,
				key->value, len - 1, handle_env );
			len -= strlen(buffer) + 1;
			buffer += strlen(buffer) + 1;
                }
            }
            *buffer = '\0';
            if (len <= 1)
                /*If either lpszSection or lpszKey is NULL and the supplied
                  destination buffer is too small to hold all the strings, 
                  the last string is truncated and followed by two null characters.
                  In this case, the return value is equal to cchReturnBuffer
                  minus two. */
            {
		buffer[-1] = '\0';
                return oldlen - 2;
            }
            return oldlen - len;
        }
        section = section->next;
    }
    buffer[0] = buffer[1] = '\0';
    return 0;
}


static INT PROFILE_GetSectionNames( LPSTR buffer, UINT len )
{
    LPSTR buf = buffer;
    WORD l, cursize = 0;
    PROFILESECTION *section;

    if(!buffer) return 0;

    for (section = CurProfile->section; section; section = section->next)
	if (section->name[0]) {
	    l = strlen(section->name);
	    cursize += l+1;
	    if (cursize > len+1)
		return len-2;

	    strcpy(buf, section->name);
	    buf += l+1;
	}

    *buf=0;
    buf++;
    return buf-buffer;
}


/***********************************************************************
 *           PROFILE_GetString
 *
 * Get a profile string.
 *
 * Tests with GetPrivateProfileString16, W95a,
 * with filled buffer ("****...") and section "set1" and key_name "1" valid:
 * section	key_name	def_val		res	buffer
 * "set1"	"1"		"x"		43	[data]
 * "set1"	"1   "		"x"		43	[data]		(!)
 * "set1"	"  1  "'	"x"		43	[data]		(!)
 * "set1"	""		"x"		1	"x"
 * "set1"	""		"x   "		1	"x"		(!)
 * "set1"	""		"  x   "	3	"  x"		(!)
 * "set1"	NULL		"x"		6	"1\02\03\0\0"
 * "set1"	""		"x"		1	"x"
 * NULL		"1"		"x"		0	""		(!)
 * ""		"1"		"x"		1	"x"
 * NULL		NULL		""		0	""
 * 
 *	
 */
static INT PROFILE_GetString( LPCSTR section, LPCSTR key_name,
			      LPCSTR def_val, LPSTR buffer, UINT len )
{
    PROFILEKEY *key = NULL;

    if(!buffer) return 0;

    if (!def_val) def_val = "";
    if (key_name && key_name[0])
    {
        key = PROFILE_Find( &CurProfile->section, section, key_name, FALSE );
        PROFILE_CopyEntry( buffer, (key && key->value) ? key->value : def_val,
                           len, FALSE );
        TRACE("('%s','%s','%s'): returning '%s'\n",
                         section, key_name, def_val, buffer );
        return strlen( buffer );
    }
    if (key_name && !(key_name[0]))
      /* Win95 returns 0 on keyname "". Tested with Likse32 bon 000227 */
      return 0;
    if (section && section[0])
        return PROFILE_GetSection(CurProfile->section, section, buffer, len,
				FALSE, FALSE);
    buffer[0] = '\0';
    return 0;
}


/***********************************************************************
 *           PROFILE_SetString
 *
 * Set a profile string.
 */
static BOOL PROFILE_SetString( LPCSTR section_name, LPCSTR key_name,
			       LPCSTR value )
{
    if (!key_name)  /* Delete a whole section */
    {
        TRACE("('%s')\n", section_name);
        CurProfile->changed |= PROFILE_DeleteSection( &CurProfile->section,
                                                      section_name );
        return TRUE;         /* Even if PROFILE_DeleteSection() has failed,
                                this is not an error on application's level.*/
    }
    else if (!value)  /* Delete a key */
    {
        TRACE("('%s','%s')\n",
                         section_name, key_name );
        CurProfile->changed |= PROFILE_DeleteKey( &CurProfile->section,
                                                  section_name, key_name );
        return TRUE;          /* same error handling as above */
    }
    else  /* Set the key value */
    {
        PROFILEKEY *key = PROFILE_Find( &CurProfile->section, section_name,
                                        key_name, TRUE );
        TRACE("('%s','%s','%s'): \n",
                         section_name, key_name, value );
        if (!key) return FALSE;
        if (key->value)
        {
	    /* strip the leading spaces. We can safely strip \n\r and 
	     * friends too, they should not happen here anyway. */
	    while (PROFILE_isspace(*value)) value++;

            if (!strcmp( key->value, value ))
            {
                TRACE("  no change needed\n" );
                return TRUE;  /* No change needed */
            }
            TRACE("  replacing '%s'\n", key->value );
            HeapFree( GetProcessHeap(), 0, key->value );
        }
        else TRACE("  creating key\n" );
        key->value = HeapAlloc( GetProcessHeap(), 0, strlen(value)+1 );
        strcpy( key->value, value );
        CurProfile->changed = TRUE;
    }
    return TRUE;
}


/***********************************************************************
 *           PROFILE_GetWineIniString
 *
 * Get a config string from the wine.ini file.
 */
int PROFILE_GetWineIniString( const char *section, const char *key_name,
                              const char *def, char *buffer, int len )
{
    char tmp[PROFILE_MAX_LINE_LEN];
    HKEY hkey;
    DWORD err;

    if (!(err = RegOpenKeyA( wine_profile_key, section, &hkey )))
    {
        DWORD type;
        DWORD count = sizeof(tmp);
        err = RegQueryValueExA( hkey, key_name, 0, &type, tmp, &count );
        RegCloseKey( hkey );
    }
    PROFILE_CopyEntry( buffer, err ? def : tmp, len, TRUE );
    TRACE( "('%s','%s','%s'): returning '%s'\n", section, key_name, def, buffer );
    return strlen(buffer);
}


/***********************************************************************
 *           PROFILE_EnumWineIniString
 *
 * Get a config string from the wine.ini file.
 */
BOOL PROFILE_EnumWineIniString( const char *section, int index,
                                char *name, int name_len, char *buffer, int len )
{
    char tmp[PROFILE_MAX_LINE_LEN];
    HKEY hkey;
    DWORD err, type;
    DWORD count = sizeof(tmp);

    if (RegOpenKeyA( wine_profile_key, section, &hkey )) return FALSE;
    err = RegEnumValueA( hkey, index, name, (DWORD*)&name_len, NULL, &type, tmp, &count );
    RegCloseKey( hkey );
    if (!err)
    {
        PROFILE_CopyEntry( buffer, tmp, len, TRUE );
        TRACE( "('%s',%d): returning '%s'='%s'\n", section, index, name, buffer );
    }
    return !err;
}


/***********************************************************************
 *           PROFILE_GetWineIniInt
 *
 * Get a config integer from the wine.ini file.
 */
int PROFILE_GetWineIniInt( const char *section, const char *key_name, int def )
{
    char buffer[20];
    char *p;
    long result;

    PROFILE_GetWineIniString( section, key_name, "", buffer, sizeof(buffer) );
    if (!buffer[0]) return def;
    result = strtol( buffer, &p, 0 );
    return (p == buffer) ? 0  /* No digits at all */ : (int)result;
}


/******************************************************************************
 *
 *   int  PROFILE_GetWineIniBool(
 *      char const  *section,
 *      char const  *key_name,
 *      int  def )
 *
 *   Reads a boolean value from the wine.ini file.  This function attempts to
 *   be user-friendly by accepting 'n', 'N' (no), 'f', 'F' (false), or '0'
 *   (zero) for false, 'y', 'Y' (yes), 't', 'T' (true), or '1' (one) for
 *   true.  Anything else results in the return of the default value.
 *
 *   This function uses 1 to indicate true, and 0 for false.  You can check
 *   for existence by setting def to something other than 0 or 1 and
 *   examining the return value.
 */
int  PROFILE_GetWineIniBool(
    char const  *section,
    char const  *key_name,
    int  def )
{
    char  key_value[2];
    int  retval;

    PROFILE_GetWineIniString(section, key_name, "~", key_value, 2);

    switch(key_value[0]) {
    case 'n':
    case 'N':
    case 'f':
    case 'F':
    case '0':
	retval = 0;
	break;

    case 'y':
    case 'Y':
    case 't':
    case 'T':
    case '1':
	retval = 1;
	break;

    default:
	retval = def;
    }

    TRACE("(\"%s\", \"%s\", %s), [%c], ret %s.\n", section, key_name,
		    def ? "TRUE" : "FALSE", key_value[0],
		    retval ? "TRUE" : "FALSE");

    return retval;
}


/***********************************************************************
 *           PROFILE_LoadWineIni
 *
 * Load the old .winerc file.
 */
int PROFILE_LoadWineIni(void)
{
    OBJECT_ATTRIBUTES attr;
    UNICODE_STRING nameW;
    char buffer[MAX_PATHNAME_LEN];
    const char *p;
    FILE *f;
    HKEY hKeySW;
    DWORD disp;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    /* make sure HKLM\\Software\\Wine\\Wine exists as non-volatile key */
    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Machine\\Software\\Wine\\Wine" ) ||
        NtCreateKey( &hKeySW, KEY_ALL_ACCESS, &attr, 0, NULL, 0, &disp ))
    {
        ERR("Cannot create config registry key\n" );
        ExitProcess( 1 );
    }
    RtlFreeUnicodeString( &nameW );
    NtClose( hKeySW );

    if (!RtlCreateUnicodeStringFromAsciiz( &nameW, "Machine\\Software\\Wine\\Wine\\Config" ) ||
        NtCreateKey( &wine_profile_key, KEY_ALL_ACCESS, &attr, 0,
                     NULL, REG_OPTION_VOLATILE, &disp ))
    {
        ERR("Cannot create config registry key\n" );
        ExitProcess( 1 );
    }
    RtlFreeUnicodeString( &nameW );

    if (!CLIENT_IsBootThread()) return 1;  /* already loaded */

    if ((p = getenv( "HOME" )) != NULL)
    {
        lstrcpynA(buffer, p, MAX_PATHNAME_LEN - sizeof(PROFILE_WineIniName));
        strcat( buffer, PROFILE_WineIniName );
        if ((f = fopen( buffer, "r" )) != NULL)
        {
	    lstrcpynA(PROFILE_WineIniUsed,buffer,MAX_PATHNAME_LEN);
            goto found;
        }
    }
    else WARN("could not get $HOME value for config file.\n" );

    if (disp == REG_OPENED_EXISTING_KEY) return 1;  /* loaded by the server */

    MESSAGE( "Can't open configuration file %s/config\n",get_config_dir() );
    return 0;

 found:

    if (disp == REG_OPENED_EXISTING_KEY)
    {
        MESSAGE( "Warning: configuration loaded by the server from '%s/config',\n"
                 "         file '%s' was ignored.\n", get_config_dir(), PROFILE_WineIniUsed );
        fclose( f );
        return 1;
    }

    /* convert to the new format */
    sprintf( buffer, "%s/config", get_config_dir() );
    convert_config( f, buffer );
    fclose( f );

    MESSAGE( "The '%s' configuration file has been converted\n"
             "to the new format and saved as '%s'.\n", PROFILE_WineIniUsed, buffer );
    MESSAGE( "You should verify that the contents of the new file are correct,\n"
             "and then remove the old one and restart Wine.\n" );
    ExitProcess(0);
}


/***********************************************************************
 *           PROFILE_UsageWineIni
 *
 * Explain the wine.ini file to those who don't read documentation.
 * Keep below one screenful in length so that error messages above are
 * noticed.
 */
void PROFILE_UsageWineIni(void)
{
    MESSAGE("Perhaps you have not properly edited or created "
	"your Wine configuration file.\n");
    MESSAGE("This is '%s/config'\n", get_config_dir());
    /* RTFM, so to say */
}

/***********************************************************************
 *           PROFILE_GetStringItem
 *
 *  Convenience function that turns a string 'xxx, yyy, zzz' into 
 *  the 'xxx\0 yyy, zzz' and returns a pointer to the 'yyy, zzz'.
 */
char* PROFILE_GetStringItem( char* start )
{
    char* lpchX, *lpch;

    for (lpchX = start, lpch = NULL; *lpchX != '\0'; lpchX++ )
    {
        if( *lpchX == ',' )
        {
            if( lpch ) *lpch = '\0'; else *lpchX = '\0';
            while( *(++lpchX) )
                if( !PROFILE_isspace(*lpchX) ) return lpchX;
        }
	else if( PROFILE_isspace( *lpchX ) && !lpch ) lpch = lpchX;
	     else lpch = NULL;
    }
    if( lpch ) *lpch = '\0';
    return NULL;
}

/********************* API functions **********************************/

/***********************************************************************
 *           GetProfileInt   (KERNEL.57)
 */
UINT16 WINAPI GetProfileInt16( LPCSTR section, LPCSTR entry, INT16 def_val )
{
    return GetPrivateProfileInt16( section, entry, def_val, "win.ini" );
}


/***********************************************************************
 *           GetProfileIntA   (KERNEL32.@)
 */
UINT WINAPI GetProfileIntA( LPCSTR section, LPCSTR entry, INT def_val )
{
    return GetPrivateProfileIntA( section, entry, def_val, "win.ini" );
}

/***********************************************************************
 *           GetProfileIntW   (KERNEL32.@)
 */
UINT WINAPI GetProfileIntW( LPCWSTR section, LPCWSTR entry, INT def_val )
{
    return GetPrivateProfileIntW( section, entry, def_val, wininiW );
}

/*
 * if allow_section_name_copy is TRUE, allow the copying :
 *   - of Section names if 'section' is NULL
 *   - of Keys in a Section if 'entry' is NULL
 * (see MSDN doc for GetPrivateProfileString)
 */
static int PROFILE_GetPrivateProfileString( LPCSTR section, LPCSTR entry,
					    LPCSTR def_val, LPSTR buffer,
					    UINT16 len, LPCSTR filename,
					    BOOL allow_section_name_copy )
{
    int		ret;
    LPSTR	pDefVal = NULL;

    if (!filename) 
	filename = "win.ini";

    /* strip any trailing ' ' of def_val. */
    if (def_val)
    {
        LPSTR p = (LPSTR)&def_val[strlen(def_val)]; /* even "" works ! */

	while (p > def_val)
	{
	    p--;
	    if ((*p) != ' ')
		break;
	}
	if (*p == ' ') /* ouch, contained trailing ' ' */
	{
	    int len = (int)p - (int)def_val;
	    pDefVal = HeapAlloc(GetProcessHeap(), 0, len + 1);
	    strncpy(pDefVal, def_val, len);
	    pDefVal[len] = '\0';
	}
    }
    if (!pDefVal)
	pDefVal = (LPSTR)def_val;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
	if ((allow_section_name_copy) && (section == NULL))
            ret = PROFILE_GetSectionNames(buffer, len);
	else
	    /* PROFILE_GetString already handles the 'entry == NULL' case */
            ret = PROFILE_GetString( section, entry, pDefVal, buffer, len );
    } else {
       lstrcpynA( buffer, pDefVal, len );
       ret = strlen( buffer );
    }

    LeaveCriticalSection( &PROFILE_CritSect );

    if (pDefVal != def_val) /* allocated */
	HeapFree(GetProcessHeap(), 0, pDefVal);
    
    return ret;
}

/***********************************************************************
 *           GetPrivateProfileString   (KERNEL.128)
 */
INT16 WINAPI GetPrivateProfileString16( LPCSTR section, LPCSTR entry,
                                        LPCSTR def_val, LPSTR buffer,
                                        UINT16 len, LPCSTR filename )
{
    return PROFILE_GetPrivateProfileString( section, entry, def_val,
					    buffer, len, filename, FALSE );
}

/***********************************************************************
 *           GetPrivateProfileStringA   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileStringA( LPCSTR section, LPCSTR entry,
				     LPCSTR def_val, LPSTR buffer,
				     UINT len, LPCSTR filename )
{
    return PROFILE_GetPrivateProfileString( section, entry, def_val,
					    buffer, len, filename, TRUE );
}

/***********************************************************************
 *           GetPrivateProfileStringW   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileStringW( LPCWSTR section, LPCWSTR entry,
				     LPCWSTR def_val, LPWSTR buffer,
				     UINT len, LPCWSTR filename )
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR entryA    = HEAP_strdupWtoA( GetProcessHeap(), 0, entry );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    LPSTR def_valA  = HEAP_strdupWtoA( GetProcessHeap(), 0, def_val );
    LPSTR bufferA   = HeapAlloc( GetProcessHeap(), 0, len );
    INT ret = GetPrivateProfileStringA( sectionA, entryA, def_valA,
                                            bufferA, len, filenameA );
    if (len > 0 && !MultiByteToWideChar( CP_ACP, 0, bufferA, -1, buffer, len ))
        buffer[len-1] = 0;
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, entryA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    HeapFree( GetProcessHeap(), 0, def_valA );
    HeapFree( GetProcessHeap(), 0, bufferA);
    return ret;
}

/***********************************************************************
 *           GetProfileString   (KERNEL.58)
 */
INT16 WINAPI GetProfileString16( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                                 LPSTR buffer, UINT16 len )
{
    return PROFILE_GetPrivateProfileString( section, entry, def_val,
                                            buffer, len, "win.ini", FALSE );
}

/***********************************************************************
 *           GetProfileStringA   (KERNEL32.@)
 */
INT WINAPI GetProfileStringA( LPCSTR section, LPCSTR entry, LPCSTR def_val,
			      LPSTR buffer, UINT len )
{
    return PROFILE_GetPrivateProfileString( section, entry, def_val,
				            buffer, len, "win.ini", TRUE );
}

/***********************************************************************
 *           GetProfileStringW   (KERNEL32.@)
 */
INT WINAPI GetProfileStringW( LPCWSTR section, LPCWSTR entry,
			      LPCWSTR def_val, LPWSTR buffer, UINT len )
{
    return GetPrivateProfileStringW( section, entry, def_val,
				     buffer, len, wininiW );
}

/***********************************************************************
 *           WriteProfileString   (KERNEL.59)
 */
BOOL16 WINAPI WriteProfileString16( LPCSTR section, LPCSTR entry,
                                    LPCSTR string )
{
    return WritePrivateProfileString16( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileStringA   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileStringA( LPCSTR section, LPCSTR entry,
				 LPCSTR string )
{
    return WritePrivateProfileStringA( section, entry, string, "win.ini" );
}

/***********************************************************************
 *           WriteProfileStringW   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileStringW( LPCWSTR section, LPCWSTR entry,
                                     LPCWSTR string )
{
    return WritePrivateProfileStringW( section, entry, string, wininiW );
}


/***********************************************************************
 *           GetPrivateProfileInt   (KERNEL.127)
 */
UINT16 WINAPI GetPrivateProfileInt16( LPCSTR section, LPCSTR entry,
                                      INT16 def_val, LPCSTR filename )
{
    long result=(long)GetPrivateProfileIntA(section,entry,def_val,filename);

    if (result > 65535) return 65535;
    if (result >= 0) return (UINT16)result;
    if (result < -32768) return -32768;
    return (UINT16)(INT16)result;
}

/***********************************************************************
 *           GetPrivateProfileIntA   (KERNEL32.@)
 */
UINT WINAPI GetPrivateProfileIntA( LPCSTR section, LPCSTR entry,
				   INT def_val, LPCSTR filename )
{
    char buffer[20];
    char *p;
    long result;

    PROFILE_GetPrivateProfileString( section, entry, "",
                                     buffer, sizeof(buffer), filename, FALSE );
    if (!buffer[0]) return (UINT)def_val;
    result = strtol( buffer, &p, 0 );
    if (p == buffer) return 0;  /* No digits at all */
    return (UINT)result;
}

/***********************************************************************
 *           GetPrivateProfileIntW   (KERNEL32.@)
 */
UINT WINAPI GetPrivateProfileIntW( LPCWSTR section, LPCWSTR entry,
				   INT def_val, LPCWSTR filename )
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR entryA    = HEAP_strdupWtoA( GetProcessHeap(), 0, entry );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    UINT res = GetPrivateProfileIntA(sectionA, entryA, def_val, filenameA);
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    HeapFree( GetProcessHeap(), 0, entryA );
    return res;
}

/***********************************************************************
 *           GetPrivateProfileSection   (KERNEL.418)
 */
INT16 WINAPI GetPrivateProfileSection16( LPCSTR section, LPSTR buffer,
				        UINT16 len, LPCSTR filename )
{
    return GetPrivateProfileSectionA( section, buffer, len, filename );
}

/***********************************************************************
 *           GetPrivateProfileSectionA   (KERNEL32.@)
 */
INT WINAPI GetPrivateProfileSectionA( LPCSTR section, LPSTR buffer,
				      DWORD len, LPCSTR filename )
{
    int		ret = 0;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
        ret = PROFILE_GetSection(CurProfile->section, section, buffer, len,
				 FALSE, TRUE);
    
    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}

/***********************************************************************
 *           GetPrivateProfileSectionW   (KERNEL32.@)
 */

INT WINAPI GetPrivateProfileSectionW (LPCWSTR section, LPWSTR buffer,
				      DWORD len, LPCWSTR filename )

{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    LPSTR bufferA   = HeapAlloc( GetProcessHeap(), 0, len );
    INT ret = GetPrivateProfileSectionA( sectionA, bufferA, len, 
						filenameA );
    MultiByteToWideChar(CP_ACP,0,bufferA,ret,buffer,len);
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    HeapFree( GetProcessHeap(), 0, bufferA);
    return ret;
}

/***********************************************************************
 *           GetProfileSection   (KERNEL.419)
 */
INT16 WINAPI GetProfileSection16( LPCSTR section, LPSTR buffer, UINT16 len )
{
    return GetPrivateProfileSection16( section, buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileSectionA   (KERNEL32.@)
 */
INT WINAPI GetProfileSectionA( LPCSTR section, LPSTR buffer, DWORD len )
{
    return GetPrivateProfileSectionA( section, buffer, len, "win.ini" );
}

/***********************************************************************
 *           GetProfileSectionW   (KERNEL32.@)
 */
INT WINAPI GetProfileSectionW( LPCWSTR section, LPWSTR buffer, DWORD len )
{
    return GetPrivateProfileSectionW( section, buffer, len, wininiW );
}


/***********************************************************************
 *           WritePrivateProfileString   (KERNEL.129)
 */
BOOL16 WINAPI WritePrivateProfileString16( LPCSTR section, LPCSTR entry,
                                           LPCSTR string, LPCSTR filename )
{
    return WritePrivateProfileStringA(section,entry,string,filename);
}

/***********************************************************************
 *           WritePrivateProfileStringA   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStringA( LPCSTR section, LPCSTR entry,
					LPCSTR string, LPCSTR filename )
{
    BOOL ret = FALSE;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
    {
        if (!section && !entry && !string)
            PROFILE_ReleaseFile();  /* always return FALSE in this case */
        else
            ret = PROFILE_SetString( section, entry, string );
    }

    LeaveCriticalSection( &PROFILE_CritSect );
    return ret;
}

/***********************************************************************
 *           WritePrivateProfileStringW   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStringW( LPCWSTR section, LPCWSTR entry,
					LPCWSTR string, LPCWSTR filename )
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR entryA    = HEAP_strdupWtoA( GetProcessHeap(), 0, entry );
    LPSTR stringA   = HEAP_strdupWtoA( GetProcessHeap(), 0, string );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    BOOL res = WritePrivateProfileStringA( sectionA, entryA,
					   stringA, filenameA );
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, entryA );
    HeapFree( GetProcessHeap(), 0, stringA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    return res;
}

/***********************************************************************
 *           WritePrivateProfileSection   (KERNEL.416)
 */
BOOL16 WINAPI WritePrivateProfileSection16( LPCSTR section, 
				 	    LPCSTR string, LPCSTR filename )
{
    return WritePrivateProfileSectionA( section, string, filename );
}

/***********************************************************************
 *           WritePrivateProfileSectionA   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileSectionA( LPCSTR section, 
					 LPCSTR string, LPCSTR filename )
{
    BOOL ret = FALSE;
    LPSTR p ;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
        if (!section && !string)
            PROFILE_ReleaseFile();  /* always return FALSE in this case */
        else if (!string) /* delete the named section*/
	    ret = PROFILE_SetString(section,NULL,NULL);
        else {
	    PROFILE_DeleteAllKeys(section);
	    ret = TRUE;
	    while(*string) {
	        LPSTR buf = HeapAlloc( GetProcessHeap(), 0, strlen(string)+1 );
                strcpy( buf, string );
                if((p=strchr( buf, '='))){
                    *p='\0';
                    ret = PROFILE_SetString( section, buf, p+1 );
                    
                }
                HeapFree( GetProcessHeap(), 0, buf );
                string += strlen(string)+1;
            }
            
        }
    }

    LeaveCriticalSection( &PROFILE_CritSect );
    return ret;
}

/***********************************************************************
 *           WritePrivateProfileSectionW   (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileSectionW( LPCWSTR section,
					 LPCWSTR string, LPCWSTR filename)

{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR stringA   = HEAP_strdupWtoA( GetProcessHeap(), 0, string );
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    BOOL res = WritePrivateProfileSectionA( sectionA, stringA, filenameA );
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, stringA );
    HeapFree( GetProcessHeap(), 0, filenameA );
    return res;
}

/***********************************************************************
 *           WriteProfileSection   (KERNEL.417)
 */
BOOL16 WINAPI WriteProfileSection16( LPCSTR section, LPCSTR keys_n_values)
{
    return WritePrivateProfileSection16( section, keys_n_values, "win.ini");
}

/***********************************************************************
 *           WriteProfileSectionA   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileSectionA( LPCSTR section, LPCSTR keys_n_values)
                                     
{
    return WritePrivateProfileSectionA( section, keys_n_values, "win.ini");
}

/***********************************************************************
 *           WriteProfileSectionW   (KERNEL32.@)
 */
BOOL WINAPI WriteProfileSectionW( LPCWSTR section, LPCWSTR keys_n_values)
{
   return (WritePrivateProfileSectionW (section,keys_n_values, wininiW));
}

/***********************************************************************
 *           GetPrivateProfileSectionNames   (KERNEL.143)
 */
WORD WINAPI GetPrivateProfileSectionNames16( LPSTR buffer, WORD size,
                                             LPCSTR filename )
{
    WORD ret = 0;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename ))
	ret = PROFILE_GetSectionNames(buffer, size);

    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}


/***********************************************************************
 *           GetProfileSectionNames   (KERNEL.142)
 */
WORD WINAPI GetProfileSectionNames16( LPSTR buffer, WORD size)

{
    return (GetPrivateProfileSectionNames16 (buffer,size,"win.ini"));
}


/***********************************************************************
 *           GetPrivateProfileSectionNamesA  (KERNEL32.@)
 */
DWORD WINAPI GetPrivateProfileSectionNamesA( LPSTR buffer, DWORD size,
					     LPCSTR filename)

{
 return (GetPrivateProfileSectionNames16 (buffer,size,filename));
}


/***********************************************************************
 *           GetPrivateProfileSectionNamesW  (KERNEL32.@)
 */
DWORD WINAPI GetPrivateProfileSectionNamesW( LPWSTR buffer, DWORD size,
					     LPCWSTR filename)

{
   LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
   LPSTR bufferA   = HeapAlloc( GetProcessHeap(), 0, size);

   INT ret = GetPrivateProfileSectionNames16 (bufferA, size, filenameA);
   if (size > 0 && !MultiByteToWideChar( CP_ACP, 0, bufferA, -1, buffer, size ))
        buffer[size-1] = 0;
   HeapFree( GetProcessHeap(), 0, bufferA);
   HeapFree( GetProcessHeap(), 0, filenameA );

   return ret;
}

/***********************************************************************
 *           GetPrivateProfileStruct (KERNEL.407)
 */
BOOL16 WINAPI GetPrivateProfileStruct16(LPCSTR section, LPCSTR key, 
 				        LPVOID buf, UINT16 len, LPCSTR filename)
{
    return GetPrivateProfileStructA( section, key, buf, len, filename );
}

/***********************************************************************
 *           GetPrivateProfileStructA (KERNEL32.@)
 *
 * Should match Win95's behaviour pretty much
 */
BOOL WINAPI GetPrivateProfileStructA (LPCSTR section, LPCSTR key, 
				      LPVOID buf, UINT len, LPCSTR filename)
{
    BOOL	ret = FALSE;

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) {
        PROFILEKEY *k = PROFILE_Find ( &CurProfile->section, section, key, FALSE);
	if (k) {
	    TRACE("value (at %p): '%s'\n", k->value, k->value);
	    if (((strlen(k->value) - 2) / 2) == len)
	    {
		LPSTR end, p;
		BOOL valid = TRUE;
		CHAR c;
		DWORD chksum = 0;

	        end  = k->value + strlen(k->value); /* -> '\0' */
	        /* check for invalid chars in ASCII coded hex string */
	        for (p=k->value; p < end; p++)
		{
                    if (!isxdigit(*p))
		    {
			WARN("invalid char '%c' in file '%s'->'[%s]'->'%s' !\n",
                             *p, filename, section, key);
		        valid = FALSE;
		        break;
		    }
		}
		if (valid)
		{
		    BOOL highnibble = TRUE;
		    BYTE b = 0, val;
		    LPBYTE binbuf = (LPBYTE)buf;
		    
	            end -= 2; /* don't include checksum in output data */
	            /* translate ASCII hex format into binary data */
                    for (p=k->value; p < end; p++)
            	    {
	        	c = toupper(*p);
			val = (c > '9') ?
				(c - 'A' + 10) : (c - '0');

			if (highnibble)
		    	    b = val << 4;
			else
			{
		    	    b += val;
		    	    *binbuf++ = b; /* feed binary data into output */
		    	    chksum += b; /* calculate checksum */
			}
			highnibble ^= 1; /* toggle */
            	    }
		    /* retrieve stored checksum value */
		    c = toupper(*p++);
		    b = ( (c > '9') ? (c - 'A' + 10) : (c - '0') ) << 4;
		    c = toupper(*p);
		    b +=  (c > '9') ? (c - 'A' + 10) : (c - '0');
	            if (b == (chksum & 0xff)) /* checksums match ? */
                        ret = TRUE;
                }
            }
	}
    }
    LeaveCriticalSection( &PROFILE_CritSect );

    return ret;
}

/***********************************************************************
 *           GetPrivateProfileStructW (KERNEL32.@)
 */
BOOL WINAPI GetPrivateProfileStructW (LPCWSTR section, LPCWSTR key,
				      LPVOID buffer, UINT len, LPCWSTR filename)
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR keyA      = HEAP_strdupWtoA( GetProcessHeap(), 0, key);
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    LPSTR bufferA   = HeapAlloc( GetProcessHeap(), 0, len );

    INT ret = GetPrivateProfileStructA( sectionA, keyA, bufferA,
					len, filenameA );
    if (len > 0 && !MultiByteToWideChar( CP_ACP, 0, bufferA, -1, buffer, len ))
        ((LPWSTR)buffer)[len-1] = 0;
    HeapFree( GetProcessHeap(), 0, bufferA);
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, keyA );
    HeapFree( GetProcessHeap(), 0, filenameA );

    return ret;
}



/***********************************************************************
 *           WritePrivateProfileStruct (KERNEL.406)
 */
BOOL16 WINAPI WritePrivateProfileStruct16 (LPCSTR section, LPCSTR key, 
	LPVOID buf, UINT16 bufsize, LPCSTR filename)
{
    return WritePrivateProfileStructA( section, key, buf, bufsize, filename );
}

/***********************************************************************
 *           WritePrivateProfileStructA (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStructA (LPCSTR section, LPCSTR key, 
                                        LPVOID buf, UINT bufsize, LPCSTR filename)
{
    BOOL ret = FALSE;
    LPBYTE binbuf;
    LPSTR outstring, p;
    DWORD sum = 0;

    if (!section && !key && !buf)  /* flush the cache */
        return WritePrivateProfileStringA( NULL, NULL, NULL, filename );

    /* allocate string buffer for hex chars + checksum hex char + '\0' */
    outstring = HeapAlloc( GetProcessHeap(), 0, bufsize*2 + 2 + 1);
    p = outstring;
    for (binbuf = (LPBYTE)buf; binbuf < (LPBYTE)buf+bufsize; binbuf++) {
      *p++ = hex[*binbuf >> 4];
      *p++ = hex[*binbuf & 0xf];
      sum += *binbuf;
    }
    /* checksum is sum & 0xff */
    *p++ = hex[(sum & 0xf0) >> 4];
    *p++ = hex[sum & 0xf];
    *p++ = '\0';

    EnterCriticalSection( &PROFILE_CritSect );

    if (PROFILE_Open( filename )) 
        ret = PROFILE_SetString( section, key, outstring );

    LeaveCriticalSection( &PROFILE_CritSect );

    HeapFree( GetProcessHeap(), 0, outstring );

    return ret;
}

/***********************************************************************
 *           WritePrivateProfileStructW (KERNEL32.@)
 */
BOOL WINAPI WritePrivateProfileStructW (LPCWSTR section, LPCWSTR key,
					LPVOID buf, UINT bufsize, LPCWSTR filename)
{
    LPSTR sectionA  = HEAP_strdupWtoA( GetProcessHeap(), 0, section );
    LPSTR keyA      = HEAP_strdupWtoA( GetProcessHeap(), 0, key);
    LPSTR filenameA = HEAP_strdupWtoA( GetProcessHeap(), 0, filename );
    INT ret = WritePrivateProfileStructA( sectionA, keyA, buf, bufsize,
					  filenameA );
    HeapFree( GetProcessHeap(), 0, sectionA );
    HeapFree( GetProcessHeap(), 0, keyA );
    HeapFree( GetProcessHeap(), 0, filenameA );

    return ret;
}


/***********************************************************************
 *           WriteOutProfiles   (KERNEL.315)
 */
void WINAPI WriteOutProfiles16(void)
{
    EnterCriticalSection( &PROFILE_CritSect );
    PROFILE_FlushFile();
    LeaveCriticalSection( &PROFILE_CritSect );
}

/***********************************************************************
 *           CloseProfileUserMapping   (KERNEL32.@)
 */
BOOL WINAPI CloseProfileUserMapping(void) {
    FIXME("(), stub!\n");
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
