/*
 *  Copyright 2000 Martin Strau� <mys@faveve.uni-stuttgart.de>
 *
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
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "audacious/plugin.h"
#include "audacious/util.h"
#include <glib.h>
#include <string.h>

#define MIN_BPM 1 
#define MAX_BPM 512

static InputPlugin metronom_ip;

static gboolean going;
static gboolean audio_error;
static GThread *play_thread;

struct metronom_struct {
	gint bpm;
	gint num;
	gint den;
	gint id;
};
typedef struct metronom_struct metronom_t;

#define tact_id_max 12
gint tact_id[tact_id_max][2]=
	{
		{1,1},
		{2,2},
		{3,2},
		{4,2},
		{2,4},
		{3,4},
		{4,4},
		{6,4},
		{2,8},
		{3,8},
		{4,8},
		{6,8}
	};
#define tact_form_max 8
gdouble tact_form[tact_id_max][tact_form_max]=
	{
		{1.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.0,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.5,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.6,0.5,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.0,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.5,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.6,0.5,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.5,0.6,0.5,0.5,0.0,0.0},
		{1.0,0.5,0.0,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.5,0.0,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.6,0.5,0.0,0.0,0.0,0.0},
		{1.0,0.5,0.5,0.6,0.5,0.5,0.0,0.0}
	};

static void metronom_about(void)
{
	static GtkWidget *box;
	box = xmms_show_message(
		"About Metronom",
		"A Tact Generator by Martin Strau� <mys@faveve.uni-stuttgart.de>\n\nTo use it, add a URL: tact://beats*num/den\ne.g. tact://77 to play 77 beats per minute\nor   tact://60*3/4 to play 60 bpm in 3/4 tacts", "Ok",
		FALSE, NULL, NULL);
	gtk_signal_connect(GTK_OBJECT(box), "destroy",
			   GTK_SIGNAL_FUNC(gtk_widget_destroyed), &box);
}

static int metronom_is_our_file(char *filename)
{
	if (!strncmp(filename, "tact://", 7))
		return TRUE;
	return FALSE;
}

#define BUF_SAMPLES 512
#define BUF_BYTES BUF_SAMPLES * 2
#define MAX_AMPL (GINT16_TO_LE((1<<15) - 1))

static void* play_loop(void *arg)
{
	gint16 data[BUF_SAMPLES];
	metronom_t *pmetronom=(metronom_t *)arg;
	gint i;
	
	gint16 t = 0,tact;
	gint16 datagoal = 0;
	gint16 datamiddle = 0;
	gint16 datacurrent = datamiddle;
	gint16 datalast = datamiddle;
	gint16 data_form[tact_form_max];
	gint num;	

	tact = 60*44100/pmetronom->bpm;
	/* prepare weighted amplitudes */
	for(num=0;num<pmetronom->num;num++){
		data_form[num]=MAX_AMPL*tact_form[pmetronom->id][num];
	}

	num=0;
	while (going)
	{
		for (i = 0; i < BUF_SAMPLES; i++){
			if(t==tact){
				t=0;
				datagoal = data_form[num];
				}
			else if(t==10) {
				datagoal = -data_form[num];
			}
			else if(t==25) {
				datagoal = data_form[num];
				/* circle through weighted amplitudes */
				num++;
				if(num==pmetronom->num)num=0;
			} 
			/* makes curve a little bit smoother  */ 
			data[i]=(datalast+datacurrent+datagoal)/3;
			datalast=datacurrent;
			datacurrent=data[i];
			if(t > 35)
			datagoal=(datamiddle+7*datagoal)/8;
			t++;
		}
		while(metronom_ip.output->buffer_free() < BUF_BYTES && going)
			xmms_usleep(30000);
		if (going)
			produce_audio(metronom_ip.output->written_time(), FMT_S16_LE, 1, BUF_BYTES, data, &going);
	}
	/* Make sure the output plugin stops prebuffering */
	free(arg);
	metronom_ip.output->buffer_free();
	metronom_ip.output->buffer_free();
	g_thread_exit(NULL);
}

static void metronom_play(char *filename)
{
	gchar *name;
	size_t count;	
	metronom_t *pmetronom;
	gint flag,id;

	pmetronom=(metronom_t *)malloc(sizeof(metronom_t));
	if(!pmetronom)return;

	count=sscanf(filename, "tact://%d*%d/%d", &pmetronom->bpm,&pmetronom->num,&pmetronom->den);
	if (count != 1 && count !=3)return;
	if(!(pmetronom->bpm >= MIN_BPM && pmetronom->bpm <= MAX_BPM))return;
	pmetronom->id=0;
	if(count==1){
		pmetronom->num=1;
		pmetronom->den=1;
	} else {
		if(pmetronom->num==0 || pmetronom->den==0)return;
		flag=FALSE;
		for(id=0;(id<tact_id_max && (!flag));id++){
			if(pmetronom->num==tact_id[id][0] && pmetronom->den==tact_id[id][1]){
				flag=TRUE;
				pmetronom->id=id;
			}
		}
		if(!flag)return;
	}
	
	going = TRUE;
	audio_error = FALSE;
	if (metronom_ip.output->open_audio(FMT_S16_LE, 44100, 1) == 0)
	{
		audio_error = TRUE;
		going = FALSE;
		return;
	}
	if(pmetronom->num==1 && pmetronom->den==1){
		name = g_strdup_printf("Tact generator: %d bpm", pmetronom->bpm);
	} else {
		name = g_strdup_printf("Tact generator: %d bpm %d/%d", pmetronom->bpm,pmetronom->num,pmetronom->den);
	}
	metronom_ip.set_info(name, -1, 16 * 44100, 44100, 1);
	g_free(name);
	play_thread = g_thread_create((GThreadFunc)play_loop, pmetronom, TRUE, NULL);
}

static void metronom_stop(void)
{
	if (going)
	{
		going = FALSE;
		g_thread_join(play_thread);
		metronom_ip.output->close_audio();
	}
}

static void metronom_pause(short paused)
{
	metronom_ip.output->pause(paused);
}

static int metronom_get_time(void)
{
	if (audio_error)
		return -2;
	if (!going && !metronom_ip.output->buffer_playing())
		return -1;
	return metronom_ip.output->output_time();
}

static void metronom_song_info(char *filename, char **title, int *length)
{
	metronom_t metronom;
	metronom_t *pmetronom=&metronom;
	size_t count;
	gint flag,id;
	*length = -1;
	*title = NULL;

	count=sscanf(filename, "tact://%d*%d/%d", &pmetronom->bpm,&pmetronom->num,&pmetronom->den);
	if (count != 1 && count !=3)return;
	if(!(pmetronom->bpm >= MIN_BPM && pmetronom->bpm <= MAX_BPM))return;

	if (count == 1)	{
		pmetronom->num=1;
		pmetronom->den=1;
		pmetronom->id=0;
	} else {
		if(pmetronom->num==0 || pmetronom->den==0)return;
		flag=FALSE;
		for(id=0;(id<tact_id_max && (!flag));id++){
			if(pmetronom->num==tact_id[id][0] && pmetronom->den==tact_id[id][1])flag=TRUE;
		}
		if(!flag)return;
		else pmetronom->id=id;
	}

	if(pmetronom->num==1 && pmetronom->den==1){
		*title = g_strdup_printf("Tact generator: %d bpm", pmetronom->bpm);
	} else {
		*title = g_strdup_printf("Tact generator: %d bpm %d/%d", pmetronom->bpm,pmetronom->num,pmetronom->den);
	}
}
		
	
	
static InputPlugin metronom_ip = 
{
	NULL,
	NULL,
	"Tact Generator " VERSION,
	NULL,
	metronom_about,
	NULL,
	metronom_is_our_file,
	NULL,
	metronom_play,
	metronom_stop,
	metronom_pause,
	NULL,
	NULL,
	metronom_get_time,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	metronom_song_info,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};

InputPlugin *get_iplugin_info(void)
{
	return &metronom_ip;
}
