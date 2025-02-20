/*
 * Copyright 2014-2019, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description:
 * This translation unit takes a source screen of a set number of lines
 * of attributes+cells, and with a reference set of fonts, rasters into
 * a pixel buffer or indirected slices of a packed atlas. These come in
 * a tight packed byte array in the native byte order.
 *
 * For correct rendering, some responsibilities are placed on the source that
 * packs the buffer so that all affected cells are actually part of a delta
 * update. Relevant such cases are:
 *
 * 1. 'italic' attribute may kern across cells, and should thus continue across
 * all cells.
 *
 * 2. shaped lines needs to invalidate full lines as the cursor behavior /
 * position will be dependent on the contents.
 *
 * 3. shaped lines needs to use the offset lookup function in order to
 * translate mouse input actions or the cursor / selection will be wrong.
 *
 * 4. Essential characters and substitutions used as 'graphics' need to be
 * verified against the fonts so that the codepoints exist, otherwise swapped
 * for a valid replacement.
 *
 * 5. Scrolling need to update all relevant lines in the region to scroll,
 *    only the step and direction of the first scroll- marked line will be
 *    respected (and applies to all subseqent scroll- tagged lines).
 */

/* the raster cell is 12 byte:
 * 3 bytes front_color
 * 3 bytes back_color
 * 1 byte  attribute bitmap
 * 1 byte  reserved
 * 4 bytes glyph-index or ucs4 code
 *
 * we are not overly concerned with the whole length, fixed size trumps
 * runlength here - outer layers should compress if needed.
 *
 * attribute bits:
 * bit 0: bold
 * bit 1: underline
 * bit 2: italic
 * bit 3: strikethrough
 * bit 4: shape break
 * bit 5: cursor
 * bit 7: 'ignore'
 */
static const size_t raster_cell_sz = 12;

enum cell_attr {
	CATTR_BOLD = 0,
	CATTR_UNDERLINE = 1,
	CATTR_ITALIC = 2,
	CATTR_STRIKETHROUGH = 3,
	CATTR_BREAK = 4,
	CATTR_CURSOR = 5,
	CATTR_SKIP = 7
};

enum raster_content {
/* monospaces, left to right, 1 data cell to 1 visible cell on a virtual grid */
	LINE_NORMAL = 0,

/* simplified bidirectional for shaped text that flow right to left, but maintain the
 * monospace property of NORMAL */
	LINE_RTL = 1, /* process right to left */

/* line is 'shaped', processing needs a per-line resolving function to understand at which
 * visible offset a certain window coordinate will map to, which, in turn, requires the
 * font(s) currently in use to be available */
	LINE_SHAPED = 2
};

struct __attribute__((packed)) tui_raster_line {
	uint16_t start_line;
	uint16_t ncells;
	uint16_t offset;
	uint8_t content_dir;
	uint8_t scroll_dir;
	uint8_t line_state;
};

enum cursor_states {
	CURSOR_NONE = 0,
	CURSOR_INACTIVE = 1,
	CURSOR_ACTIVE = 2,
	CURSOR_BLINK = 3
};

/*
 * lines and cells must match the actual provided contents and contents
 * size or there will be a validation fault when submitting the buffer.
 */
struct __attribute__((packed)) tui_raster_header {
	uint16_t lines;
	uint16_t cells;
	uint8_t direction;
	uint16_t flags;
	uint8_t bgc[4];

/* cursor state will be applied to cells with a cursor- bit set,
 * color, shape etc. may be overridden and drawn in a user- configured way */
	uint8_t cursor_state;
};

struct tui_font {
	union {
		struct tui_pixelfont* bitmap;
		TTF_Font* truetype;
	};

/* vector selects subtype, descriptor is kept for re-open / inherit */
	bool vector;
	int fd;
	int hint;
};

/* Build a new raster context based on the provided set of fonts,
 * this needs to be reset/rebuilt on font changes */
struct tui_raster_context* tui_raster_setup(size_t cell_w, size_t cell_h);

/* Resolve the [pixel_x, line-row] to cell- offset on line, if such
 * a resolution can be performed. This requires a line that has previously
 * been drawn with the SHAPED attribute set for the offsets to be available. */
void tui_raster_offset(
	struct tui_raster_context* ctx, size_t px_x, size_t row, size_t* offset);

/*
 * Set font- slots to use for rasterization. This will alias [src]
 * and they are expected to remain until replaced via a call to setfont.
 */
void tui_raster_setfont(
	struct tui_raster_context* ctx, struct tui_font** src, size_t n_fonts);

/*
 * Render / signal into the specified context.
 */
int tui_raster_render(struct tui_raster_context* ctx,
	struct arcan_shmif_cont* dst, uint8_t* buf, size_t buf_sz, bool delta);

/* Called when the cell size has unexpectedly changed
 */
void tui_raster_cell_size(struct tui_raster_context* ctx, size_t w, size_t h);

/*
 * Synch the raster state into the agp_store
 */
#ifdef HAVE_ARCAN_AGP
void tui_raster_renderagp(struct tui_raster_context* ctx,
	struct agp_vstore* dst, uint8_t* buf, size_t buf_sz, bool delta);
#endif

/*
 * Free any buffers and resources bound to the raster.
 */
void tui_raster_free(struct tui_raster_context*);
