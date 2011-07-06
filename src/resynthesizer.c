/*
  The Resynthesizer - A GIMP plug-in for resynthesizing textures
  
  Copyright (C) 2010  Lloyd Konneker
  Copyright (C) 2000 2008  Paul Francis Harrison
  Copyright (C) 2002  Laurent Despeyroux
  Copyright (C) 2002  David Rodríguez García

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
 
/*
Notes:

The selection:

In prior versions, you could pass the same layer as the target and corpus.
Since there is only one selection, the selection was the target and the inverse
of the selection was the corpus.  But if you wanted to pass a different image and layer
as the corpus, you needed to invert the selection in that image.

This feature was a source of confusion for users and programmers.
Here, this feature is abolished.  The selection in the corpus layer is the corpus,
not the inverse of the selection.

This only eliminates one use: synthesizing a selection from the inverse of the selection
in the same drawable.  If you need to do that, copy the drawable to another image and
create a selection there that is the inverse of the selection in the original.
The heal selection plugin does that for you.

The alpha:

In prior versions the alpha was treated like a color channel, and matched during resynthesis.
Transparent pixels (which Gimp arbitrarily gives the color black in some circumstances)
were not distinguished.  In certain cases  with transparency, transparent pixels were synthesized
into the target, as good matches for black.

Here, we don't match the alpha channel between target and corpus.
We don't generate any alpha in the target, instead we leave the target alpha unaltered.
We use the alpha to determine what pixels are in the target and corpus, 
(similar to a selection mask.)
Any totally transparent pixel in the target selection IS synthesized,
I.E. a color is generated (but since it is totally transparent, you don't see it.)
Any partially transparent target pixel is also synthesized, except as stated,
the alpha is not matched (so colors from opaque areas of the corpus
could be synthesized into partially transparent areas of the target.)
Any totally transparent pixel in the corpus is not in the corpus, i.e. never matched.
Any partially transparent pixel in the corpus is a candidate for matching.
A color from a partially transparent pixel in the corpus could be synthesized
into an opaque area of the target.
Again, the transparency of the target is retained even as new colors are synthesized.

Tiling: (see parameters horizontal and vertical tiling)
This means we synthesize a target that is *seamlessly* tileable.
We treat the target as a sphere, wrapping a coord outside the target around
to the opposite side.  See wrap_or_clip.
It doesn't make tiles in the target, it makes a target that is suitable as a tile.
*/

// Bring in alternative code: experimental, debugging, etc.
// #define ANIMATE    // Animate image while processing, for debugging.
// #define DEBUG

#include "buildSwitches.h"


/*
Uncomment this before release.  I'm not sure if it the build environment
defines it on the command line to gcc.
Also, uncomment when using splint.
Leave it commented for development and testing, to enable assertions.
*/
// #define G_DISABLE_ASSERT      // To disable g_assert macro, uncomment this.

#include "../config.h" // GNU buildtools local configuration
#include "plugin-intl.h" // i18n macros

#include <libgimp/gimp.h>
#include <glib/gprintf.h>

/* Shared with resynth-gui plugin, resynthesizer engine plugin, and engine. */
#include "resynth-constants.h"

/*
True header files: types, function prototypes, and in-line functions only.
No definitions of non in-line functions or data.
*/
// FIXME need to include glibProxy.h here so everything else uses glibless Map?
#ifdef USE_GLIB_PROXY
	#include "glibProxy.h"
#endif
#include "map.h"
#include "mapIndex.h"
#include "engineParams.h"
#include "engine.h"

/* Source included, not compiled separately. Is separate to reduce file sizes and later, coupling. */
#include "adaptGimp.h"
#include "resynth-parameters.h" // Depends on engine.h

/* See below for more includes. */

/* 
Macro to cleanup for abort. 
Return value is already PDB_ERROR.
Return an error string to Gimp, which will display an alert dialog.
Also log message in case engine is called non-interactively.
Note this must be used in the scope of nreturn_vals and value, in main().
*/
#define ERROR_RETURN(message)   { \
  detach_drawables(drawable, corpus_drawable, map_in_drawable, map_out_drawable); \
  *nreturn_vals           = 2; \
  values[1].type          = GIMP_PDB_STRING; \
  values[1].data.d_string = message ; \
  g_debug(message); \
  return; \
  }


static void
progress(gchar * message)
{
  gimp_progress_init(message);
  gimp_progress_update(0.0);
#ifdef DEBUG
  /* To console.  On Windows, it annoyingly opens a console.  
  On Unix it dissappears unless console already open.
  */
  g_printf(message);  
  g_printf("\n");
#endif
}



/* Return count of color channels, exclude alpha and any other channels. */
// FIXME called from engine WAS static
guint
count_color_channels(GimpDrawable *drawable)
{
  GimpImageType type = gimp_drawable_type(drawable->drawable_id);
  switch(type)
  {
    case GIMP_RGB_IMAGE:
    case GIMP_RGBA_IMAGE:
      return 3;
    case GIMP_GRAY_IMAGE:
    case GIMP_GRAYA_IMAGE:
      return 1;
    default:
      g_assert(FALSE);
  }
  return 0;
}

// Func to set pixelel indices
#include "imageFormat.h"

/*
Return whether drawables have the same base type.
*/
static gboolean
equal_basetypes(
  GimpDrawable *first_drawable,
  GimpDrawable *second_drawable
  )
{
  /* !!! Not drawable->bpp minus one if alpha, because there  might be other channels. */
  return (count_color_channels(first_drawable) == count_color_channels(second_drawable));
}



/* 
Update Gimp image from local pixmap. Canonical postlude for plugins.
!!! Called in the postlude but also for debugging: animate results during processing.
*/
static void 
post_results_to_gimp(GimpDrawable *drawable) 
{
  pixmap_to_drawable(image, drawable, FIRST_PIXELEL_INDEX);   // our pixels to region
  gimp_drawable_flush(drawable);    // regions back to core
  gimp_drawable_merge_shadow(drawable->drawable_id,TRUE);   // temp buffers merged
  gimp_drawable_update(drawable->drawable_id,0,0,image.width,image.height);
  gimp_displays_flush();
}



static void
detach_drawables(
  GimpDrawable * out,
  GimpDrawable * in,
  GimpDrawable * out_map,
  GimpDrawable * in_map
  )
{
  if (out)
    gimp_drawable_detach(out);
  if (in)
    gimp_drawable_detach(in);
  if (out_map)
    gimp_drawable_detach(out_map);
  if (in_map)
    gimp_drawable_detach(in_map);
}

// FIXME
// #include "engine.h"
#ifdef ADAPT_SIMPLE
  #include "imageBuffer.h"
  #include "adaptSimple.h"
  #include "adaptGimpSimple.h"
#endif

/* 
Plugin main.
This adapts the texture synthesis engine to a Gimp plugin.
*/

static void run(
  const gchar *     name,
  gint              nparams,
	const GimpParam * param,
	gint *            nreturn_vals,
	GimpParam **      return_vals)
{
  static GimpParam values[2];   /* Gimp return values. !!! Allow 2: status and error message. */
  Parameters parameters;
  
  GimpDrawable *drawable = NULL;
  GimpDrawable *corpus_drawable = NULL; 
  GimpDrawable *map_in_drawable= NULL; 
  GimpDrawable *map_out_drawable= NULL; 
  gboolean ok, with_map;
  
  #ifdef DEBUG
  gimp_message_set_handler(1); // To console instead of GUI
  start_time = clock();
  #endif
  
  // internationalization i18n
  // Note these constants are defined in the build environment.
  bindtextdomain (GETTEXT_PACKAGE, LOCALEDIR);
  #ifdef HAVE_BIND_TEXTDOMAIN_CODESET
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  #endif
  textdomain (GETTEXT_PACKAGE);   // Equivalent to: textdomain("resynthesizer") ;  

  *nreturn_vals = 1;
  *return_vals = values;
  values[0].type = GIMP_PDB_STATUS;
  values[0].data.d_status = GIMP_PDB_EXECUTION_ERROR; /* Unless everything succeeds. */
  
  drawable = gimp_drawable_get(param[2].data.d_drawable);

  /* Check image type (could be called non-interactive) */
  if (!gimp_drawable_is_rgb(drawable->drawable_id) &&
      !gimp_drawable_is_gray(drawable->drawable_id)) 
  {
    ERROR_RETURN(_("Incompatible image mode."));
  }


  /* Deal with run mode */
  ok = FALSE;
  switch(param[0].data.d_int32) 
  {
    case GIMP_RUN_INTERACTIVE :
      ok = get_last_parameters(&parameters,drawable->drawable_id, RESYNTH_ENGINE_PDB_NAME);
      gimp_message("Resynthesizer engine should not be called interactively");
      /* But keep going with last (or default) parameters, really no harm. */
      break;
    case GIMP_RUN_NONINTERACTIVE :
      ok = get_parameters_from_list(&parameters, nparams, param); 
      break;
    case GIMP_RUN_WITH_LAST_VALS :
      ok = get_last_parameters(&parameters,drawable->drawable_id, RESYNTH_ENGINE_PDB_NAME); 
      break;
  }

  if (!ok) 
  {
    ERROR_RETURN(_("Resynthesizer failed to get parameters."));
  }
  
  /* Limit neighbours parameter to size allocated. */
  if (parameters.neighbours > RESYNTH_MAX_NEIGHBORS )
    parameters.neighbours = RESYNTH_MAX_NEIGHBORS;
  
  // dump_parameters(&parameters);
  
  corpus_drawable = gimp_drawable_get(parameters.corpus_id);
  
  /* The target and corpus must have the same base type.
  In earlier version, they must have the same bpp.
  But now we don't compare the alphas, so they can differ in presence of alpha.
  */
  if (! equal_basetypes(drawable, corpus_drawable) )
  {
    ERROR_RETURN(_("The input texture and output image must have the same number of color channels."));
  }
  
  
  with_map = (parameters.input_map_id != -1 && parameters.output_map_id != -1);
  /* If only one map is passed, it is ignored quietly. */
  map_in_drawable=0;
  map_out_drawable=0;

  if (with_map) 
  {
    map_in_drawable = gimp_drawable_get(parameters.input_map_id);
    map_out_drawable = gimp_drawable_get(parameters.output_map_id);
    /* All these can be wrong at the same time.  
    Forego userfriendliness for ease of programming: abort on first error
    */
    if ( ! equal_basetypes(map_in_drawable, map_out_drawable) )
    {
      /* Maps need the same base type. Formerly needed the same bpp. */
      ERROR_RETURN(_("The input and output maps must have the same mode"));
    } 
    if (map_in_drawable->width != corpus_drawable->width || 
               map_in_drawable->height != corpus_drawable->height) 
    {
      ERROR_RETURN(_("The input map should be the same size as the input texture image"));
    } 
    if (map_out_drawable->width != drawable->width || 
               map_out_drawable->height != drawable->height) 
    {
      ERROR_RETURN(_("The output map should be the same size as the output image"));
    }
  }

  /* 
  The engine should not be run interactively so no need to store last values. 
  I.E. the meaning of "last" is "last values set by user interaction".
  */
  
  #ifdef ADAPT_SIMPLE
    /* Adapt Gimp to an engine with a simpler interface. */
    setDefaultParams(&parameters);
    ImageBuffer imageBuffer;
    ImageBuffer maskBuffer;
    
    // Image adaption requires format indices
    prepareImageFormatIndices(drawable, corpus_drawable, with_map, map_in_drawable);
    // g_printf("Here2\n");
    adaptGimpToSimple(drawable, &imageBuffer, &maskBuffer);  // From Gimp to simple
    g_printf("Here3\n");
    adaptSimpleAPI(&imageBuffer, &maskBuffer);        // From simple to existing engine API
    
  #else
    // Image adaption requires format indices
    prepareImageFormatIndices(drawable, corpus_drawable, with_map, map_in_drawable);
    
    /* target/context adaption */
    fetch_image_mask_map(drawable, &image, total_bpp, &image_mask, MASK_TOTALLY_SELECTED, 
      map_out_drawable, map_start_bip);
    
    /*  corpus adaption */
    fetch_image_mask_map(corpus_drawable, &corpus, total_bpp, &corpus_mask, MASK_TOTALLY_SELECTED, 
        map_in_drawable, map_start_bip);
    free_map(&corpus_mask);
    // !!! Note the engine yet uses image_mask 
  #endif
  
  // After possible adaption, check size again
  g_assert(image.width * image.height); // Image is not empty
  g_assert(corpus.width * corpus.height); // Corpus is not empty
  
  // Done with adaption: now main image data in canonical pixmaps, etc.
  int result = engine(parameters);
  // ANIMATE int result = engine(parameters, drawable);
  if (result == 1)
  {
    ERROR_RETURN(_("The texture source is empty. Does any selection include non-transparent pixels?"));
  }
  else if  (result == 2 )
  {
    ERROR_RETURN(_("The output layer is empty. Does any selection have visible pixels in the active layer?"));
  }
  // else must be 0, success
  
  // Normal post-process adaption follows

  /* dump_target_points(); */ /* detailed debugging. */
  // print_post_stats();
  
  // Update Gimp image from local pixmap
  // Note this works even for test harness where ADAPT_SIMPLE
  // but then it does NOT test returning results in buffer.
  
  /* 
  TODO to test that antiAdaptImage works need more here:
  But antiAdaptImage has already been tested once on the incoming side.
  So no compelling need to test it again here.
  #ifdef ADAPT_SIMPLE   
    antiAdaptImage(
      ImageBuffer * imageBuffer,              // OUT image: target or corpus drawable
      Map          *pixmap,             // IN NON-rowpadded pixmap
      guint        offset,              // IN Offset in destination pixel
      guint        pixelel_count        // IN count pixelels to move
      )
    postBufferToGimp(foo)   // Not written yet
  #else
  */
  post_results_to_gimp(drawable); 
  
  /* Clean up */
  detach_drawables(drawable, corpus_drawable, map_in_drawable, map_out_drawable);
  gimp_progress_end();
  values[0].data.d_status = GIMP_PDB_SUCCESS;
} 

/* PDB registration and MAIN() */
#include "resynth-pdb.h"

