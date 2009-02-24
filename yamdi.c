/*
 * yamdi.c
 *
 * Copyright (c) 2007, Ingo Oppermann
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * -----------------------------------------------------------------------------
 *
 * Compile with:
 * gcc yamdi.c -o yamdi -Wall -O2
 *
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FLV_UI32(x) (int)(((x[0]) << 24) + ((x[1]) << 16) + ((x[2]) << 8) + (x[3]))
#define FLV_UI24(x) (int)(((x[0]) << 16) + ((x[1]) << 8) + (x[2]))
#define FLV_UI16(x) (int)(((x[0]) << 8) + (x[1]))
#define FLV_UI8(x) (int)((x))

#define FLV_AUDIODATA	8
#define FLV_VIDEODATA	9
#define FLV_SCRIPTDATAOBJECT	18

#define FLV_H263VIDEOPACKET	2
#define FLV_SCREENVIDEOPACKET	3
#define	FLV_VP6VIDEOPACKET	4
#define	FLV_VP6ALPHAVIDEOPACKET	5
#define FLV_SCREENV2VIDEOPACKET	6

#define YAMDI_VERSION	"1.1"

#ifndef MAP_NOCORE
	#define MAP_NOCORE	0
#endif

typedef struct {
	int hasKeyframes;
	int hasVideo;
	int hasAudio;
	int hasMetadata;
	int hasCuePoints;
	int canSeekToEnd;

	double audiocodecid;
	double audiosamplerate;
	double audiodatarate;
	double audiosamplesize;
	double audiodelay;
	int stereo;

	double videocodecid;
	double framerate;
	double videodatarate;
	double height;
	double width;

	double datasize;
	double audiosize;
	double videosize;
	double filesize;

	double lasttimestamp;
	double lastkeyframetimestamp;
	double lastkeyframelocation;

	int keyframes;
	double *filepositions;
	double *times;
	double duration;

	char metadatacreator[256];
	char creator[256];

	int onmetadatalength;
	int metadatasize;
} FLVMetaData_t;

FLVMetaData_t flvmetadata;

typedef struct {
	unsigned char signature[3];
	unsigned char version;
	unsigned char flags;
	unsigned char headersize[4];
} FLVFileHeader_t;

typedef struct {
	unsigned char type;
	unsigned char datasize[3];
	unsigned char timestamp[3];
	unsigned char timestamp_ex;
	unsigned char streamid[3];
} FLVTag_t;

typedef struct {
	unsigned char flags;
} FLVAudioData_t;

typedef struct {
	unsigned char flags;
} FLVVideoData_t;

void initFLVMetaData(const char *creator);
size_t writeFLVMetaData(FILE *fp);

void readFLVFirstPass(char *flv, size_t streampos, size_t filesize);
void readFLVSecondPass(char *flv, size_t streampos, size_t filesize);
void readFLVH263VideoPacket(const unsigned char *h263);
void readFLVScreenVideoPacket(const unsigned char *sv);
void readFLVVP62VideoPacket(const unsigned char *vp62);
void readFLVVP62AlphaVideoPacket(const unsigned char *vp62a);

size_t writeFLVScriptDataValueArray(FILE *fp, const char *name, size_t len);
size_t writeFLVScriptDataECMAArray(FILE *fp, const char *name, size_t len);
size_t writeFLVScriptDataVariableArray(FILE *fp, const char *name);
size_t writeFLVScriptDataVariableArrayEnd(FILE *fp);
size_t writeFLVScriptDataValueString(FILE *fp, const char *name, const char *value);
size_t writeFLVScriptDataValueBool(FILE *fp, const char *name, int value);
size_t writeFLVScriptDataValueDouble(FILE *fp, const char *name, double value);
size_t writeFLVScriptDataObject(FILE *fp);

size_t writeFLVPreviousTagSize(FILE *fp, size_t datasize);

size_t writeFLVScriptDataString(FILE *fp, const char *s);
size_t writeFLVScriptDataLongString(FILE *fp, const char *s);
size_t writeFLVBool(FILE *fp, int value);
size_t writeFLVDouble(FILE *fp, double v);

void writeFLV(FILE *fp, char *flv, size_t streampos, size_t filesize);
void writeFLVHeader(FILE *fp);

void print_usage(void);

int main(int argc, char *argv[]) {
	FILE *fp_infile = NULL, *fp_outfile = NULL, *devnull;
	int c;
	char *flv, *infile, *outfile, *creator;
	unsigned int i;
	size_t filesize = 0, streampos, metadatasize;
	struct stat sb;
	FLVFileHeader_t *flvfileheader;

	opterr = 0;

	infile = NULL;
	outfile = NULL;
	creator = NULL;

	while((c = getopt(argc, argv, "i:o:c:h")) != -1) {
		switch(c) {
			case 'i':
				if(infile == NULL) {
					infile = optarg;

					if(outfile != NULL) {
						if(!strcmp(infile, outfile)) {
							fprintf(stderr, "Input file and output file must not be the same.\n");
							exit(1);
						}
					}

					if(stat(infile, &sb) == -1) {
						fprintf(stderr, "Couldn't stat on %s.\n", infile);
						exit(1);
					}

					filesize = sb.st_size;

					fp_infile = fopen(infile, "rb");
					if(fp_infile == NULL) {
						fprintf(stderr, "Couldn't open %s.\n", infile);
						exit(1);
					}
				}

				break;
			case 'o':
				if(outfile == NULL) {
					outfile = optarg;

					if(infile != NULL) {
						if(!strcmp(infile, outfile)) {
							fprintf(stderr, "Input file and output file must not be the same.\n");
							exit(1);
						}
					}

					if(strcmp(outfile, "-")) {
						fp_outfile = fopen(outfile, "wb");
						if(fp_outfile == NULL) {
							fprintf(stderr, "Couldn't open %s.\n", outfile);
							exit(1);
						}
					}
					else
						fp_outfile = stdout;
				}
				break;
			case 'c':
				creator = optarg;
				break;
			case 'h':
				print_usage();
				exit(1);
				break;
			case ':':
				fprintf(stderr, "The option -%c expects a parameter. -h for help.\n", optopt);
				exit(1);
				break;
			case '?':
				fprintf(stderr, "Unknown option: -%c. -h for help.\n", optopt);
				exit(1);
				break;
			default:
				print_usage();
				exit(1);
				break;
		}
	}

	if(fp_infile == NULL || fp_outfile == NULL) {
		fprintf(stderr, "Please provide an input file and an output file. -h for help.\n");
		exit(1);
	}

	// mmap von infile erstellen
	flv = mmap(NULL, filesize, PROT_READ, MAP_NOCORE | MAP_PRIVATE, fileno(fp_infile), 0);
	if(flv == NULL) {
		fprintf(stderr, "Couldn't load %s.\n", infile);
		exit(1);
	}

	if(strncmp(flv, "FLV", 3)) {
		fprintf(stderr, "The input file is not a FLV.\n");
		exit(1);
	}

	// Metadata initialisieren
	initFLVMetaData(creator);

	flvfileheader = (FLVFileHeader_t *)flv;

	// Die Position des 1. Tags im FLV bestimmen (Header + PrevTagSize0)
	streampos = FLV_UI32(flvfileheader->headersize) + 4;

	// Das FLV einlesen und Informationen fuer die Metatags extrahieren
	readFLVFirstPass(flv, streampos, filesize);

	devnull = fopen("/dev/null", "wb");
	if(devnull == NULL) {
		fprintf(stderr, "Couldn't open NULL device.\n");
		exit(1);
	}

	metadatasize = writeFLVMetaData(devnull);
	fclose(devnull);

	// Falls es Keyframes hat, muss ein 2. Durchgang fuer den Keyframeindex gemacht werden
	if(flvmetadata.hasKeyframes == 1) {
		readFLVSecondPass(flv, streampos, filesize);

		// Die Filepositions korrigieren
		for(i = 0; i < flvmetadata.keyframes; i++)
			flvmetadata.filepositions[i] += (double)(sizeof(FLVFileHeader_t) + 4 + metadatasize);

		flvmetadata.lastkeyframelocation = flvmetadata.filepositions[flvmetadata.keyframes - 1];
	}

	// filesize = FLVFileHeader + PreviousTagSize0 + MetadataSize + DataSize
	flvmetadata.filesize = (double)(sizeof(FLVFileHeader_t) + 4 + metadatasize + flvmetadata.datasize);

	writeFLV(fp_outfile, flv, streampos, filesize);

	return 0;
}

void writeFLV(FILE *fp, char *flv, size_t streampos, size_t filesize) {
	size_t datasize;
	FLVTag_t *flvtag;

	writeFLVHeader(fp);
	writeFLVMetaData(fp);

	for(;;) {
		if(streampos + sizeof(FLVTag_t) > filesize)
			break;

		flvtag = (FLVTag_t *)&flv[streampos];

		// Die Groesse des Tags (Header + Data) + PreviousTagSize
		datasize = sizeof(FLVTag_t) + FLV_UI24(flvtag->datasize) + 4;

		if(streampos + datasize > filesize)
			break;

		if(flvtag->type == FLV_VIDEODATA || flvtag->type == FLV_AUDIODATA)
			fwrite(&flv[streampos], datasize, 1, fp);

		streampos += datasize;
	}

	return;
}

void writeFLVHeader(FILE *fp) {
	char *t;
	size_t size;
	FLVFileHeader_t flvheader;

	t = (char *)&flvheader;
	bzero(t, sizeof(FLVFileHeader_t));

	flvheader.signature[0] = 'F';
	flvheader.signature[1] = 'L';
	flvheader.signature[2] = 'V';

	flvheader.version = 1;

	if(flvmetadata.hasAudio == 1)
		flvheader.flags |= 0x4;

	if(flvmetadata.hasVideo == 1)
		flvheader.flags |= 0x1;

	size = sizeof(FLVFileHeader_t);
	flvheader.headersize[0] = ((size >> 24) & 0xff);
	flvheader.headersize[1] = ((size >> 16) & 0xff);
	flvheader.headersize[2] = ((size >> 8) & 0xff);
	flvheader.headersize[3] = (size & 0xff);

	fwrite(t, sizeof(FLVFileHeader_t), 1, fp);

	writeFLVPreviousTagSize(fp, 0);

	return;
}

void readFLVSecondPass(char *flv, size_t streampos, size_t filesize) {
	int i;
	size_t datasize, datapos;
	FLVTag_t *flvtag;
	FLVVideoData_t *flvvideo;

	if(flvmetadata.keyframes == 0)
		return;

	i = 0;
	datapos = 0;

	for(;;) {
		if(streampos + sizeof(FLVTag_t) > filesize)
			break;

		flvtag = (FLVTag_t *)&flv[streampos];

		// TagHeader + TagData + PreviousTagSize
		datasize = sizeof(FLVTag_t) + FLV_UI24(flvtag->datasize) + 4;

		if(streampos + datasize > filesize)
			break;

		if(flvtag->type == FLV_VIDEODATA) {
			flvvideo = (FLVVideoData_t *)&flv[streampos + sizeof(FLVTag_t)];

			if(((flvvideo->flags >> 4) & 1) == 1) {
				flvmetadata.filepositions[i] = (double)datapos;
				flvmetadata.times[i] = (double)((flvtag->timestamp_ex << 24) + (flvtag->timestamp[0] << 16) + (flvtag->timestamp[1] << 8) + flvtag->timestamp[2]) / 1000.0;
				i++;
			}
		}

		streampos += datasize;

		if(flvtag->type == FLV_VIDEODATA || flvtag->type == FLV_AUDIODATA)
			datapos += datasize;
	}

	return;
}

void readFLVFirstPass(char *flv, size_t streampos, size_t filesize) {
	size_t datasize, videosize = 0, audiosize = 0;
	size_t videotags = 0, audiotags = 0;
	FLVTag_t *flvtag;
	FLVAudioData_t *flvaudio;
	FLVVideoData_t *flvvideo;

	for(;;) {
		if(streampos + sizeof(FLVTag_t) > filesize)
			break;

		flvtag = (FLVTag_t *)&flv[streampos];

		// TagHeader + TagData + PreviousTagSize
		datasize = sizeof(FLVTag_t) + FLV_UI24(flvtag->datasize) + 4;

		if(streampos + datasize > filesize)
			break;

		if(flvtag->type == FLV_AUDIODATA) {
			flvmetadata.datasize += (double)datasize;
			// datasize - PreviousTagSize
			flvmetadata.audiosize += (double)(datasize - 4);

			audiosize += FLV_UI24(flvtag->datasize);
			audiotags++;

			if(flvmetadata.hasAudio == 0) {
				flvaudio = (FLVAudioData_t *)&flv[streampos + sizeof(FLVTag_t)];

				// Sound Codec
				flvmetadata.audiocodecid = (double)((flvaudio->flags >> 4) & 0xf);

				// Sample Rate
				switch(((flvaudio->flags >> 2) & 0x3)) {
					case 0:
						flvmetadata.audiosamplerate = 5500.0;
						break;
					case 1:
						flvmetadata.audiosamplerate = 11000.0;
						break;
					case 2:
						flvmetadata.audiosamplerate = 22000.0;
						break;
					case 3:
						flvmetadata.audiosamplerate = 44100.0;
						break;
					default:
						break;
				}

				// Sample Size
				switch(((flvaudio->flags >> 1) & 0x1)) {
					case 0:
						flvmetadata.audiosamplesize = 8.0;
						break;
					case 1:
						flvmetadata.audiosamplesize = 16.0;
						break;
					default:
						break;
				}

				// Stereo
				flvmetadata.stereo = (flvaudio->flags & 0x1);

				flvmetadata.hasAudio = 1;
			}
		}
		else if(flvtag->type == FLV_VIDEODATA) {
			flvmetadata.datasize += (double)datasize;
			// datasize - PreviousTagSize
			flvmetadata.videosize += (double)(datasize - 4);

			videosize += FLV_UI24(flvtag->datasize);
			videotags++;

			flvvideo = (FLVVideoData_t *)&flv[streampos + sizeof(FLVTag_t)];

			if(flvmetadata.hasVideo == 0) {
				// Video Codec
				flvmetadata.videocodecid = (double)(flvvideo->flags & 0xf);

				flvmetadata.hasVideo = 1;

				switch(flvvideo->flags & 0xf) {
					case FLV_H263VIDEOPACKET:
						readFLVH263VideoPacket((unsigned char *)&flv[streampos + sizeof(FLVTag_t) + sizeof(FLVVideoData_t)]);
						break;
					case FLV_SCREENVIDEOPACKET:
						readFLVScreenVideoPacket((unsigned char *)&flv[streampos + sizeof(FLVTag_t) + sizeof(FLVVideoData_t)]);
						break;
					case FLV_VP6VIDEOPACKET:
						readFLVVP62VideoPacket((unsigned char *)&flv[streampos + sizeof(FLVTag_t) + sizeof(FLVVideoData_t)]);
						break;
					case FLV_VP6ALPHAVIDEOPACKET:
						readFLVVP62AlphaVideoPacket((unsigned char *)&flv[streampos + sizeof(FLVTag_t) + sizeof(FLVVideoData_t)]);
						break;
					case FLV_SCREENV2VIDEOPACKET:
						readFLVScreenVideoPacket((unsigned char *)&flv[streampos + sizeof(FLVTag_t) + sizeof(FLVVideoData_t)]);
						break;
					default:
						break;
				}
			}

			// keyframes
			if(((flvvideo->flags >> 4) & 1) == 1) {
				flvmetadata.canSeekToEnd = 1;
				flvmetadata.keyframes++;
				flvmetadata.lastkeyframetimestamp = (double)((flvtag->timestamp_ex << 24) + (flvtag->timestamp[0] << 16) + (flvtag->timestamp[1] << 8) + flvtag->timestamp[2]) / 1000.0;
			}
			else
				flvmetadata.canSeekToEnd = 0;
		}

		flvmetadata.lasttimestamp = (double)((flvtag->timestamp_ex << 24) + (flvtag->timestamp[0] << 16) + (flvtag->timestamp[1] << 8) + flvtag->timestamp[2]) / 1000.0;

		streampos += datasize;
	}

	flvmetadata.duration = flvmetadata.lasttimestamp;

	if(flvmetadata.keyframes != 0)
		flvmetadata.hasKeyframes = 1;

	if(flvmetadata.hasKeyframes == 1) {
		flvmetadata.filepositions = (double *)calloc(flvmetadata.keyframes, sizeof(double));
		flvmetadata.times = (double *)calloc(flvmetadata.keyframes, sizeof(double));

		if(flvmetadata.filepositions == NULL || flvmetadata.times == NULL) {
			fprintf(stderr, "Not enough memory for the keyframe index.\n");
			exit(1);
		}
	}

	// Framerate
	if(videotags != 0)
		flvmetadata.framerate = (double)videotags / flvmetadata.duration;

	// Videodatarate (kb/s)
	if(videosize != 0)
		flvmetadata.videodatarate = (double)(videosize * 8) / 1024.0 / flvmetadata.duration;

	// Audiodatarate (kb/s)
	if(audiosize != 0)
		flvmetadata.audiodatarate = (double)(audiosize * 8) / 1024.0 / flvmetadata.duration;

	return;
}

void readFLVH263VideoPacket(const unsigned char *h263) {
	int startcode, picturesize;

	// 8bit  |pppppppp|pppppppp|pvvvvvrr|rrrrrrss|swwwwwww|whhhhhhh|h
	// 16bit |pppppppp|pppppppp|pvvvvvrr|rrrrrrss|swwwwwww|wwwwwwww|whhhhhhh|hhhhhhhh|h

	startcode = FLV_UI24(h263) >> 7;
	if(startcode != 1)
		return;

	picturesize = ((h263[3] & 0x3) << 1) + ((h263[4] >> 7) & 0x1);

	switch(picturesize) {
		case 0: // Custom 8bit
			flvmetadata.width = (double)(((h263[4] & 0x7f) << 1) + ((h263[5] >> 7) & 0x1));
			flvmetadata.height = (double)(((h263[5] & 0x7f) << 1) + ((h263[6] >> 7) & 0x1));
			break;
		case 1: // Custom 16bit
			flvmetadata.width = (double)(((h263[4] & 0x7f) << 9) + (h263[5] << 1) + ((h263[6] >> 7) & 0x1));
			flvmetadata.height = (double)(((h263[6] & 0x7f) << 9) + (h263[7] << 1) + ((h263[8] >> 7) & 0x1));
			break;
		case 2: // CIF
			flvmetadata.width = 352.0;
			flvmetadata.height = 288.0;
			break;
		case 3: // QCIF
			flvmetadata.width = 176.0;
			flvmetadata.height = 144.0;
			break;
		case 4: // SQCIF
			flvmetadata.width = 128.0;
			flvmetadata.height = 96.0;
			break;
		case 5:
			flvmetadata.width = 320.0;
			flvmetadata.height = 240.0;
			break;
		case 6:
			flvmetadata.width = 160.0;
			flvmetadata.height = 120.0;
			break;
		default:
			break;
	}

	return;
}

void readFLVScreenVideoPacket(const unsigned char *sv) {
	// |1111wwww|wwwwwwww|2222hhhh|hhhhhhhh|

	flvmetadata.width = (double)((sv[0] << 4) + sv[1]);
	flvmetadata.height = (double)((sv[2] << 4) + sv[3]);

	return;
}

void readFLVVP62VideoPacket(const unsigned char *vp62) {
	flvmetadata.width = (double)(vp62[4] * 16 - (vp62[0] >> 4));
	flvmetadata.height = (double)(vp62[3] * 16 - (vp62[0] & 0x0f));

	return;
}

void readFLVVP62AlphaVideoPacket(const unsigned char *vp62a) {
	flvmetadata.width = (double)(vp62a[7] * 16 - (vp62a[0] >> 4));
	flvmetadata.height = (double)(vp62a[6] * 16 - (vp62a[0] & 0x0f));

	return;
}

void initFLVMetaData(const char *creator) {
	char *t;

	t = (char *)&flvmetadata;
	bzero(t, sizeof(FLVMetaData_t));

	flvmetadata.hasMetadata = 1;

	if(creator != NULL)
		strncpy(flvmetadata.creator, creator, sizeof(flvmetadata.creator));

	strncpy(flvmetadata.metadatacreator, "Yet Another Metadata Injector for FLV - Version " YAMDI_VERSION "\0", sizeof(flvmetadata.metadatacreator));

	return;
}

size_t writeFLVMetaData(FILE *fp) {
	FLVTag_t flvtag;
	int i;
	size_t length = 0, datasize = 0;
	char *t;

	if(fp == NULL)
		return -1;

	// Zuerst ein ScriptDataObject Tag schreiben

	// Alles auf 0 setzen
	t = (char *)&flvtag;
	bzero(t, sizeof(FLVTag_t));

	// Tag Type
	flvtag.type = FLV_SCRIPTDATAOBJECT;

	flvtag.datasize[0] = ((flvmetadata.metadatasize >> 16) & 0xff);
	flvtag.datasize[1] = ((flvmetadata.metadatasize >> 8) & 0xff);
	flvtag.datasize[2] = (flvmetadata.metadatasize & 0xff);

metadatapass:
	datasize = 0;
	datasize += fwrite(t, 1, sizeof(FLVTag_t), fp);

	// ScriptDataObject
	datasize += writeFLVScriptDataObject(fp);

	// onMetaData
	datasize += writeFLVScriptDataECMAArray(fp, "onMetaData", flvmetadata.onmetadatalength);

	// creator
	if(strlen(flvmetadata.creator) != 0) {
		datasize += writeFLVScriptDataValueString(fp, "creator", flvmetadata.creator);
		length++;
	}

	// metadatacreator
	datasize += writeFLVScriptDataValueString(fp, "metadatacreator", flvmetadata.metadatacreator);
	length++;

	// hasKeyframes
	datasize += writeFLVScriptDataValueBool(fp, "hasKeyframes", flvmetadata.hasKeyframes);
	length++;

	// hasVideo
	datasize += writeFLVScriptDataValueBool(fp, "hasVideo", flvmetadata.hasVideo);
	length++;

	// hasAudio
	datasize += writeFLVScriptDataValueBool(fp, "hasAudio", flvmetadata.hasAudio);
	length++;

	// hasMetadata
	datasize += writeFLVScriptDataValueBool(fp, "hasMetadata", flvmetadata.hasMetadata);
	length++;

	// canSeekToEnd
	datasize += writeFLVScriptDataValueBool(fp, "canSeekToEnd", flvmetadata.canSeekToEnd);
	length++;

	// duration
	datasize += writeFLVScriptDataValueDouble(fp, "duration", flvmetadata.duration);
	length++;

	// datasize
	datasize += writeFLVScriptDataValueDouble(fp, "datasize", flvmetadata.datasize);
	length++;

	if(flvmetadata.hasVideo == 1) {
		// videosize
		datasize += writeFLVScriptDataValueDouble(fp, "videosize", flvmetadata.videosize);
		length++;

		// videocodecid
		datasize += writeFLVScriptDataValueDouble(fp, "videocodecid", flvmetadata.videocodecid);
		length++;

		// width
		if(flvmetadata.width != 0.0) {
			datasize += writeFLVScriptDataValueDouble(fp, "width", flvmetadata.width);
			length++;
		}

		// height
		if(flvmetadata.height != 0.0) {
			datasize += writeFLVScriptDataValueDouble(fp, "height", flvmetadata.height);
			length++;
		}

		// framerate
		datasize += writeFLVScriptDataValueDouble(fp, "framerate", flvmetadata.framerate);
		length++;

		// videodatarate
		datasize += writeFLVScriptDataValueDouble(fp, "videodatarate", flvmetadata.videodatarate);
		length++;
	}

	if(flvmetadata.hasAudio == 1) {
		// audiosize
		datasize += writeFLVScriptDataValueDouble(fp, "audiosize", flvmetadata.audiosize);
		length++;

		// audiocodecid
		datasize += writeFLVScriptDataValueDouble(fp, "audiocodecid", flvmetadata.audiocodecid);
		length++;

		// audiosamplerate
		datasize += writeFLVScriptDataValueDouble(fp, "audiosamplerate", flvmetadata.audiosamplerate);
		length++;

		// audiosamplesize
		datasize += writeFLVScriptDataValueDouble(fp, "audiosamplesize", flvmetadata.audiosamplesize);
		length++;

		// stereo
		datasize += writeFLVScriptDataValueBool(fp, "stereo", flvmetadata.stereo);
		length++;

		// audiodatarate
		datasize += writeFLVScriptDataValueDouble(fp, "audiodatarate", flvmetadata.audiodatarate);
		length++;
	}

	// filesize
	datasize += writeFLVScriptDataValueDouble(fp, "filesize", flvmetadata.filesize);
	length++;

	// lasttimestamp
	datasize += writeFLVScriptDataValueDouble(fp, "lasttimestamp", flvmetadata.lasttimestamp);
	length++;

	if(flvmetadata.hasKeyframes == 1) {
		// lastkeyframetimestamp
		datasize += writeFLVScriptDataValueDouble(fp, "lastkeyframetimestamp", flvmetadata.lastkeyframetimestamp);
		length++;

		// lastkeyframelocation
		datasize += writeFLVScriptDataValueDouble(fp, "lastkeyframelocation", flvmetadata.lastkeyframelocation);
		length++;

		// keyframes
		datasize += writeFLVScriptDataVariableArray(fp, "keyframes");
		length++;

		// filepositions
		datasize += writeFLVScriptDataValueArray(fp, "filepositions", flvmetadata.keyframes);

		for(i = 0; i < flvmetadata.keyframes; i++)
			datasize += writeFLVScriptDataValueDouble(fp, NULL, flvmetadata.filepositions[i]);

		// times
		datasize += writeFLVScriptDataValueArray(fp, "times", flvmetadata.keyframes);

		for(i = 0; i < flvmetadata.keyframes; i++)
			datasize += writeFLVScriptDataValueDouble(fp, NULL, flvmetadata.times[i]);

		// Variable Array End Object
		datasize += writeFLVScriptDataVariableArrayEnd(fp);
	}

	if(flvmetadata.onmetadatalength == 0) {
		flvmetadata.onmetadatalength = length;
		goto metadatapass;
	}

	flvmetadata.metadatasize = datasize - sizeof(FLVTag_t);

	datasize += writeFLVPreviousTagSize(fp, datasize);

	return datasize;
}

size_t writeFLVPreviousTagSize(FILE *fp, size_t datasize) {
	unsigned char length[4];

	length[0] = ((datasize >> 24) & 0xff);
	length[1] = ((datasize >> 16) & 0xff);
	length[2] = ((datasize >> 8) & 0xff);
	length[3] = (datasize & 0xff);

	fwrite(length, 1, 4, fp);

	return 4;
}

size_t writeFLVScriptDataObject(FILE *fp) {
	size_t datasize = 0;
	char type;

	type = 2;
	datasize += fwrite(&type, 1, 1, fp);

	return datasize;
}

size_t writeFLVScriptDataECMAArray(FILE *fp, const char *name, size_t len) {
	size_t datasize = 0;
	unsigned char length[4];
	char type;

	datasize += writeFLVScriptDataString(fp, name);
	type = 8;	// ECMAArray
	datasize += fwrite(&type, 1, 1, fp);

	length[0] = ((len >> 24) & 0xff);
	length[1] = ((len >> 16) & 0xff);
	length[2] = ((len >> 8) & 0xff);
	length[3] = (len & 0xff);

	datasize += fwrite(length, 1, 4, fp);

	return datasize;
}

size_t writeFLVScriptDataValueArray(FILE *fp, const char *name, size_t len) {
	size_t datasize = 0;
	unsigned char length[4];
	char type;
	
	datasize += writeFLVScriptDataString(fp, name);
	type = 10;	// Value Array
	datasize += fwrite(&type, 1, 1, fp);

	length[0] = ((len >> 24) & 0xff);
	length[1] = ((len >> 16) & 0xff);
	length[2] = ((len >> 8) & 0xff);
	length[3] = (len & 0xff);

	datasize += fwrite(length, 1, 4, fp);

	return datasize;
}

size_t writeFLVScriptDataVariableArray(FILE *fp, const char *name) {
	size_t datasize = 0;
	char type;

	datasize += writeFLVScriptDataString(fp, name);
	type = 3;	// Variable Array
	datasize += fwrite(&type, 1, 1, fp);

	return datasize;
}

size_t writeFLVScriptDataVariableArrayEnd(FILE *fp) {
	size_t datasize = 0;
	unsigned char length[3];

	length[0] = 0;
	length[1] = 0;
	length[2] = 9;

	datasize += fwrite(length, 1, 3, fp);

	return datasize;
}

size_t writeFLVScriptDataValueString(FILE *fp, const char *name, const char *value) {
	size_t datasize = 0;
	char type;

	if(name != NULL)
		datasize += writeFLVScriptDataString(fp, name);

	type = 2;	// DataString
	datasize += fwrite(&type, 1, 1, fp);
	datasize += writeFLVScriptDataString(fp, value);

	return datasize;
}

size_t writeFLVScriptDataValueBool(FILE *fp, const char *name, int value) {
	size_t datasize = 0;
	char type;

	if(name != NULL)
		datasize += writeFLVScriptDataString(fp, name);

	type = 1;	// Bool
	datasize += fwrite(&type, 1, 1, fp);
	datasize += writeFLVBool(fp, value);

	return datasize;
}

size_t writeFLVScriptDataValueDouble(FILE *fp, const char *name, double value) {
	size_t datasize = 0;
	char type;

	if(name != NULL)
		datasize += writeFLVScriptDataString(fp, name);

	type = 0;	// Double
	datasize += fwrite(&type, 1, 1, fp);
	datasize += writeFLVDouble(fp, value);

	return datasize;
}

size_t writeFLVScriptDataString(FILE *fp, const char *s) {
	size_t datasize = 0, len;
	unsigned char length[2];

	len = strlen(s);

	if(len > 0xffff)
		datasize += writeFLVScriptDataLongString(fp, s);
	else {
		length[0] = ((len >> 8) & 0xff);
		length[1] = (len & 0xff);

		datasize += fwrite(length, 1, 2, fp);
		datasize += fwrite(s, 1, len, fp);
	}

	return datasize;
}

size_t writeFLVScriptDataLongString(FILE *fp, const char *s) {
	size_t datasize = 0, len;
	unsigned char length[4];

	len = strlen(s);

	if(len > 0xffffffff)
		len = 0xffffffff;

	length[0] = ((len >> 24) & 0xff);
	length[1] = ((len >> 16) & 0xff);
	length[2] = ((len >> 8) & 0xff);
	length[3] = (len & 0xff);

	datasize += fwrite(length, 1, 4, fp);
	datasize += fwrite(s, 1, len, fp);

	return datasize;
}

size_t writeFLVBool(FILE *fp, int value) {
	size_t datasize = 0;
	unsigned char b;

	b = (value & 1);

	datasize += fwrite(&b, 1, 1, fp);

	return datasize;
}

size_t writeFLVDouble(FILE *fp, double value) {
	union {
		unsigned char dc[8];
		double dd;
	} d;
	unsigned char b[8];
	size_t datasize = 0;

	d.dd = value;

	b[0] = d.dc[7];
	b[1] = d.dc[6];
	b[2] = d.dc[5];
	b[3] = d.dc[4];
	b[4] = d.dc[3];
	b[5] = d.dc[2];
	b[6] = d.dc[1];
	b[7] = d.dc[0];

	datasize += fwrite(b, 1, 8, fp);

	return datasize;
}

void print_usage(void) {
	fprintf(stderr, "NAME\n");
	fprintf(stderr, "\tyamdi -- Yet Another Metadata Injector for FLV\n");
	fprintf(stderr, "\tVersion: " YAMDI_VERSION "\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "SYNOPSIS\n");
	fprintf(stderr, "\tyamdi -i input file -o output file [-c creator] [-h]\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "DESCRIPTION\n");
	fprintf(stderr, "\tyamdi is a metadata injector for FLV files.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\tOptions:\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-i\tThe source FLV file.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-o\tThe resulting FLV file with the metatags. If the\n");
	fprintf(stderr, "\t\toutput file is '-' the FLV file will be written to\n");
	fprintf(stderr, "\t\tstdout.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-c\tA string that will be written into the creator tag.\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "\t-h\tThis description.\n");
	fprintf(stderr, "\n");

	fprintf(stderr, "COPYRIGHT\n");
	fprintf(stderr, "\t(c) 2007 Ingo Oppermann\n");
	fprintf(stderr, "\n");
	return;
}
