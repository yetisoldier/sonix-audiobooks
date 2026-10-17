#include "src/system/library/metadata.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
	song_metadata_t m = {0};
	assert(strcmp(metadata_shown_artist(NULL), "") == 0);
	assert(strcmp(metadata_shown_artist(&m), "") == 0);
	snprintf(m.album_artist, sizeof(m.album_artist), "Album Author");
	m.compilation = true;
	assert(strcmp(metadata_shown_artist(&m), "Album Author") == 0);
	snprintf(m.artist, sizeof(m.artist), "Performer");
	assert(strcmp(metadata_shown_artist(&m), "Performer") == 0);
	m.compilation = false;
	assert(strcmp(metadata_shown_artist(&m), "Album Author") == 0);
	const char *aliases[] = {"Various Artists", "various", "VA", "V.A."};
	for (unsigned i = 0; i < sizeof(aliases) / sizeof(aliases[0]); i++) {
		snprintf(m.album_artist, sizeof(m.album_artist), "%s", aliases[i]);
		assert(strcmp(metadata_shown_artist(&m), "Performer") == 0);
	}
	if (argc != 2) return 2;
	char *description = metadata_read_with_description(argv[1], &m);
	printf("%d\n%s\n%s\n%s\n%s\n%s\n%s\n", m.compilation, metadata_shown_artist(&m),
		   m.artist, m.album_artist, m.series, m.series_part, description ? description : "");
	free(description);
	return 0;
}
