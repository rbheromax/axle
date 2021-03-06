#include <std/std.h>
#include <gfx/lib/gfx.h>
#include <gfx/lib/view.h>
#include "font8x8.h"
#include <std/math.h>

//TODO configurable SSAA factor?
//#define SSAA_FACTOR 3
#define SSAA_FACTOR 0

#define BITS_IN_WORD (sizeof(uint32_t) * 8)
static inline void bitset_set(uint32_t* bitset, int idx) {
	//figure out which index in array of ints to use
	int word_idx = idx / BITS_IN_WORD;
	//get offset within word
	int offset = idx % BITS_IN_WORD;
	//turn on bit
	bitset[word_idx] |= 1 << offset;
}

static inline bool bitset_check(uint32_t* bitset, int idx) {
	//figure out which index in array of ints to use
	int word_idx = idx / BITS_IN_WORD;
	//get offset within word
	int offset = idx % BITS_IN_WORD;
	//check bit
	return ((bitset[word_idx] >> offset) & 1);
}

static uint32_t supersample_map_cache[256][CHAR_WIDTH * SSAA_FACTOR] = {{0}};
//generate supersampled bitmap of font character
static void generate_supersampled_map(uint32_t* supersample, char ch) {
	//bitset of characters which are stored in cache
	//256 / sizeof(uint32_t) = 8
	//so we need 8 uint32_t's to store all 256 possible characters
	static uint32_t present_in_cache[8] = {0};

	//use cached map if existing
	uint32_t* cached = &supersample_map_cache[(int)ch];
	if (bitset_check((uint32_t*)&present_in_cache, (int)ch)) {
		memcpy(supersample, cached, CHAR_WIDTH * SSAA_FACTOR * sizeof(uint32_t));
		return;
	}

	//for every col in SS bitmap
	for (int y = 0; y < CHAR_HEIGHT * SSAA_FACTOR; y++) {
		uint32_t ssaa_row = 0;

		//get row from sampled bitmap
		int font_row = font8x8_basic[(int)ch][y / SSAA_FACTOR];

		//for every row in SS bitmap
		for (int x = 0; x < CHAR_WIDTH * SSAA_FACTOR; x++) {
			//copy state of sampled bitmap
			if ((font_row >> (x / SSAA_FACTOR)) & 1) {
				ssaa_row |= (1 << x);
			}
		}
		supersample[y] = ssaa_row;
		//save this row in cache
		cached[y] = ssaa_row;
	}
	//mark as saved in bitset
	bitset_set((uint32_t*)&present_in_cache, (int)ch);
}

void draw_char(ca_layer* layer, char ch, int x, int y, Color color, Size font_size) {
	Point p = point_make(x, y);
	if (p.x < 0 || p.y < 0 || p.x >= layer->size.width || p.y >= layer->size.height) return;

	//find scale factor of default font size to size requested
	float scale_x = font_size.width / (float)CHAR_WIDTH;
	float scale_y = font_size.height / (float)CHAR_HEIGHT;

	uint32_t supersample[CHAR_WIDTH * SSAA_FACTOR];
	if (SSAA_FACTOR) {
		generate_supersampled_map(&supersample, ch);
	}

	Color bg_color = color_black();
	int avg_red, avg_grn, avg_blu, sum_num;
	avg_red = avg_grn = avg_blu = sum_num = 0;
	//bg_color is the average color of the rect represented by 
	//{{x, y}, {font_size.width, font_size.height}}
	for (int bg_y = y; bg_y < y + font_size.height; bg_y++) {
		for (int bg_x = x; bg_x < x + font_size.width; bg_x++) {
			uint32_t idx = ((bg_y * layer->size.width * gfx_bpp()) + (bg_x * gfx_bpp()));
			avg_red += layer->raw[idx + 2];
			avg_grn += layer->raw[idx + 1];
			avg_blu += layer->raw[idx + 0];
			sum_num++;
		}
	}
	avg_red /= sum_num;
	avg_grn /= sum_num;
	avg_blu /= sum_num;
	bg_color.val[0] = avg_red;
	bg_color.val[1] = avg_grn;
	bg_color.val[2] = avg_blu;

	for (int draw_y = 0; draw_y < font_size.height; draw_y++) {
		//get the corresponding y of default font size
		int font_y = draw_y / scale_y;
		
		for (int draw_x = 0; draw_x < font_size.width; draw_x++) {
			//corresponding x of default font size
			int font_x = draw_x / scale_x;

			//skip antialiasing?
			if (!SSAA_FACTOR) {
				uint32_t font_row = font8x8_basic[(int)ch][font_y];

				Color draw_color = color_white();
				float a = 1 - ( 0.299 * bg_color.val[0] + 
								0.587 * bg_color.val[1] + 
								0.114 * bg_color.val[2])/255;
				if (a < 0.5) draw_color = color_black();
				draw_color = color;
				if ((font_row >> font_x) & 1) {
					putpixel(layer, x + draw_x, y + draw_y, draw_color);
				}
				/*
				else {
					putpixel(layer, x + draw_x, y + draw_y, bg_color);
				}
				*/
				continue;
			}

			//antialiasing
			//holds number of 'on' pixels in supersampled region
			//'on' pixels / total pixel count in SSAA region = alpha of pixel to draw
			int on_count = 0;
			int total_count = 0;

			//go around all supersampled pixels adjacent to current and
			//(square of 9 pixels)
			//record any 'on' pixels in supersample
			for (int dx = -1; dx <= 1; dx++) {
				for (int dy = -1; dy <= 1; dy++) {
					//is this pixel valid?
					if (font_x + dx >= 0 &&
						font_y + dy >= 0 &&
						font_x + dx <= CHAR_WIDTH &&
						font_y + dy <= CHAR_HEIGHT) {
						int ssaa_x = (font_x * SSAA_FACTOR) + dx;
						int ssaa_y = (font_y * SSAA_FACTOR) + dy;

						uint32_t ssaa_row = supersample[ssaa_y];
						total_count++;
						if ((ssaa_row >> ssaa_x) & 1) {
							on_count++;
						}
					}
				}
			}

			//'on' pixels / total pixel count in SSAA region = alpha of pixel to draw
			float alpha = (float)on_count / (float)total_count;
			
			//if drawing black or white text, try to increase legibility
			/*
			if (color_equal(color, color_white()) ||
				color_equal(color, color_black())) {
				if ((bg_color.val[0] * 0.299 + 
					bg_color.val[1] * 0.587 +
					bg_color.val[2] * 0.114) > 186) {
					color = color_black();
				}
				else {
					color = color_white();
				}
			//}

			*/
			Color avg_color = color;
			uint8_t tmp = avg_color.val[0];
			avg_color.val[0] = avg_color.val[1];
			avg_color.val[1] = avg_color.val[2];
			avg_color.val[2] = tmp;

			if (alpha) {
				//set avg_color to color * alpha
				//this is a lerp of background color to text color, at alpha
				for (int i = 0; i < gfx_bpp(); i++) {
					avg_color.val[i] = lerp(bg_color.val[i], avg_color.val[i], alpha);
				}
				putpixel(layer, p.x + draw_x, p.y + draw_y, avg_color);
			}
		}
	}
}

//returns first pointer to a string that looks like a web link in 'str', 
//or NULL if none is found
static char* link_hueristic(char* str) {
	return NULL;
	static char link_stubs[2][16] = {"http",
									 "www",
	};
	char* test = NULL;
	//check if this matches any link hints
	for (uint32_t i = 0; i < sizeof(link_stubs) / sizeof(link_stubs[0]); i++) {
		test = strstr(str, link_stubs[i]);
		//match?
		if (test) {
			break;
		}
	}
	return test;
}

Size font_padding_for_size(Size s) {
	const int factor = 8;
	return size_make(s.width / factor, s.height / factor);
}

void draw_string(ca_layer* dest, char* str, Point origin, Color color, Size font_size) {
	//get a pointer to location of a web link in this string, if any
	char* link_loc = link_hueristic(str);

	int idx = 0;
	int x = origin.x;
	int y = origin.y;
	Size padding = font_padding_for_size(font_size);

	while (str[idx]) {
		bool inserting_hyphen = false;
		//do we need to break a word onto 2 lines?
		if ((x + font_size.width + padding.width + 1) >= dest->size.width) {
			//don't bother if it was puncutation anyways
			//also, don't hypenate if string is too short
			if (str[idx] != ' ' && strlen(str) > 1) {
				inserting_hyphen = true;
			}
		}
		else if (str[idx] == '\n') {
			x = 0;

			//quit if going to next line would exceed view bounds
			if ((y + font_size.height + padding.height + 1) >= dest->size.height) break;
			y += font_size.height + padding.height;
		}

		//if this is a link, draw blue
		Color draw_color = color;
		//are we currently drawing a link?
		if (link_loc && idx >= (link_loc - str)) {
			//reset link state on whitespace, newline, or if this is the last line
			if (isspace(str[idx]) || str[idx] == '\n' || str[idx+1] == '\0') {
				link_loc = NULL;
			}
			else {
				//blue for link!
				draw_color = color_make(0, 0, 0xEE);
				//underline
				//draw_hline_fast(dest, line_make(point_make(x, y), point_make(x+1, y)), draw_color, 1);
			}
		}

		if (inserting_hyphen) {
			draw_char(dest, '-', x, y, draw_color, font_size);
			//drew hyphen, continue without drawing current character in input string
			
			x = 0;

			//quit if going to next line would exceed view bounds
			if ((y + font_size.height + padding.height+ 1) >= dest->size.height) break;
			y += font_size.height + padding.height;

			continue;
		}

		draw_char(dest, str[idx], x, y, draw_color, font_size);

		x += font_size.width + padding.width;
		idx++;
	}
}
