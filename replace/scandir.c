/* scandir: Scan a directory, collecting all (selected) items into a an array.
 *
 * This code borrowed from 'libit', which can be found here:
 *
 * http://www.iro.umontreal.ca/~pinard/libit/dist/scandir/
 *
 * The original author put this code in the public domain.
 * It has been modified slightly to get rid of warnings, etc.
 *
 * Below is the email I received from pinard@iro.umontreal.ca (Fran�ois Pinard)
 * when I sent him an email asking him about the license, etc. of this
 * code which I obtained from his site.
 *
 *	-- Alan Robertson
 *	   alanr@unix.sh
 *
 **************************************************************************
 *
 * Subject:	Re: Scandir replacement function
 * Date:	18 May 2001 12:00:48 -0400
 * From:	pinard@iro.umontreal.ca (Fran�ois Pinard)
 * To:		Alan Robertson <alanr@unix.sh>
 * References:	1
 *
 *
 * [Alan Robertson]
 *
 * > Hi, I'd like to use your scandir replacement function found here:
 * > http://www.iro.umontreal.ca/~pinard/libit/dist/scandir/ But, it does
 * > not indicate authorship or licensing terms in it.  Could you tell me
 * > who wrote this code, under what license you distribute it, and whether
 * > and under what terms I may further distribute it?
 *
 * Hello, Alan.  These are (somewhat) explained in UNSHAR.HDR found in the
 * same directory.  The routines have been written by Rick Saltz (I'm not
 * completely sure of the spelling) a long while ago.  I think that nowadays,
 * Rick is better known as the main author of the nice INN package.
 *
 * ---------------------------------------------------------------------->
 * Subject:	scandir, ftw REDUX
 * Date: 	1 Jan 88 00:47:01 GMT
 * From: 	rsalz@pebbles.bbn.com
 * Newsgroups:  comp.sources.misc
 *
 *
 * Forget my previous message -- I just decided for completeness's sake to
 * implement the SysV ftw(3) routine, too.  
 *
 * To repeat, these are public-domain implementations of the SystemV ftw()
 * routine, the BSD scandir() and alphasort() routines, and documentation for
 * same.  The FTW manpage could be more readable, but so it goes.
 *
 * Anyhow, feel free to post these, and incorporate them into your existing
 * packages.  I have readdir() routiens for MSDOS and the Amiga if anyone
 *  wants them, and should have them for VMS by the end of January; let me
 *  know if you want copies.
 *
 *                        Yours in filesystems,
 *                                /r$
 *
 *                                Anyhow, feel free to post
 * ----------------------------------------------------------------------<
 *
 */

#include <port.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <stddef.h>

#ifndef NULL
# define NULL ((void *) 0)
#endif

/* Initial guess at directory allocated.  */
#define INITIAL_ALLOCATION 20

int
scandir (const char *directory_name,
	struct dirent ***array_pointer,
	int (*select_function) (const struct dirent *),

#ifdef USE_SCANDIR_COMPARE_STRUCT_DIRENT
	/* This is what the Linux man page says */
	int (*compare_function) (const struct dirent**, const struct dirent**)
#else
	/* This is what the Linux header file says ... */
	int (*compare_function) (const void *, const void *)
#endif
	);

int
scandir (const char *directory_name,
	struct dirent ***array_pointer,
	int (*select_function) (const struct dirent *),
#ifdef USE_SCANDIR_COMPARE_STRUCT_DIRENT
	/* This is what the linux man page says */
	int (*compare_function) (const struct dirent**, const struct dirent**)
#else
	/* This is what the linux header file says ... */
	int (*compare_function) (const void *, const void *)
#endif
	)
{
  DIR *directory;
  struct dirent **array;
  struct dirent *entry;
  struct dirent *copy;
  int allocated = INITIAL_ALLOCATION;
  int counter = 0;

  /* Get initial list space and open directory.  */

  if (directory = opendir (directory_name), directory == NULL)
    return -1;

  if (array = (struct dirent **) malloc (allocated * sizeof (struct dirent *)),
      array == NULL)
    return -1;

  /* Read entries in the directory.  */

  while (entry = readdir (directory), entry)
    if (select_function == NULL || (*select_function) (entry))
      {
	/* User wants them all, or he wants this one.  Copy the entry.  */

	if (copy = (struct dirent *) malloc (sizeof (struct dirent)),
	    copy == NULL)
	  {
	    closedir (directory);
	    free (array);
	    return -1;
	  }
	copy->d_ino = entry->d_ino;
	copy->d_reclen = entry->d_reclen;
	strcpy (copy->d_name, entry->d_name);

	/* Save the copy.  */

	if (counter + 1 == allocated)
	  {
	    allocated <<= 1;
	    array = (struct dirent **)
	      realloc ((char *) array, allocated * sizeof (struct dirent *));
	    if (array == NULL)
	      {
		closedir (directory);
		free (array);
		free (copy);
		return -1;
	      }
	  }
	array[counter++] = copy;
      }

  /* Close things off.  */

  array[counter] = NULL;
  *array_pointer = array;
  closedir (directory);

  /* Sort?  */

  if (counter > 1 && compare_function)
    qsort ((char *) array, counter, sizeof (struct dirent)
  	,	(int (*)(const void *, const void *))(compare_function));

  return counter;
}
