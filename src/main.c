/*
 * main.c: entry point for xsigtool
 * Creation date: Wed Jan 11 20:35:22 2017
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <xsigtool.h>

#include <sigutils/sampling.h>
#include <sigutils/ncqo.h>
#include <sigutils/iir.h>
#include <sigutils/agc.h>
#include <sigutils/pll.h>
#include <sigutils/detect.h>
#include <sigutils/sigutils.h>

#include "constellation.h"
#include "waterfall.h"
#include "source.h"
#include "spectrum.h"

#define SCREEN_WIDTH  640
#define SCREEN_HEIGHT 480

struct xsig_interface {
  xsig_waterfall_t *wf;
  xsig_spectrum_t *s;
  su_channel_detector_t *cd;
};

SUPRIVATE void
xsigtool_onacquire(struct xsig_source *source, void *private)
{
  unsigned int i;
  struct xsig_interface *iface = (struct xsig_interface *) private;

  xsig_waterfall_feed(iface->wf, source->fft);
  xsig_spectrum_feed(iface->s, source->fft);

  for (i = 0; i < source->params.window_size; ++i)
    su_channel_detector_feed(iface->cd, source->window[i]);
}

SUBOOL
su_modem_set_xsig_source(
    su_modem_t *modem,
    const char *path,
    struct xsig_source **instance)
{
  su_block_t *xsig_source_block = NULL;
  const uint64_t *samp_rate = NULL;
  struct xsig_source_params params;

  params.file = path;
  params.onacquire = NULL;
  params.private = NULL;
  params.window_size = 512;
  params.onacquire = xsigtool_onacquire;
  params.raw_iq = SU_FALSE;

  if (strcmp(path + strlen(path) - 4, ".raw") == 0) {
    params.raw_iq = SU_TRUE;
    params.samp_rate = 250000;
  }

  if ((xsig_source_block = xsig_source_create_block(&params)) == NULL)
    goto fail;

  if ((samp_rate = su_block_get_property_ref(
      xsig_source_block,
      SU_PROPERTY_TYPE_INTEGER,
      "samp_rate")) == NULL) {
    SU_ERROR("failed to acquire xsig source sample rate\n");
    goto fail;
  }

  if ((*instance = su_block_get_property_ref(
      xsig_source_block,
      SU_PROPERTY_TYPE_OBJECT,
      "instance")) == NULL) {
    SU_ERROR("failed to acquire xsig source instance\n");
    goto fail;
  }

  if (!su_modem_register_block(modem, xsig_source_block)) {
    SU_ERROR("failed to register wav source\n");
    su_block_destroy(xsig_source_block);
    goto fail;
  }

  if (!su_modem_set_int(modem, "samp_rate", *samp_rate)) {
    SU_ERROR("failed to set modem sample rate\n");
    goto fail;
  }

  if (!su_modem_set_source(modem, xsig_source_block))
    goto fail;

  return SU_TRUE;

fail:
  if (xsig_source_block != NULL)
    su_block_destroy(xsig_source_block);

  return SU_FALSE;
}

su_modem_t *
xsig_modem_init(const char *file, struct xsig_source **instance)
{
  su_modem_t *modem = NULL;

  if ((modem = su_modem_new("qpsk")) == NULL) {
    fprintf(stderr, "xsig_modem_init: failed to initialize QPSK modem\n", file);
    return NULL;
  }

  if (!su_modem_set_xsig_source(modem, file, instance)) {
    fprintf(
        stderr,
        "xsig_modem_init: failed to set modem wav source to %s\n",
        file);
    su_modem_destroy(modem);
    return NULL;
  }

  su_modem_set_bool(modem, "abc", SU_FALSE);
  su_modem_set_bool(modem, "afc", SU_FALSE);

  su_modem_set_int(modem, "mf_span", 6);
  su_modem_set_float(modem, "baud", 468);
  su_modem_set_float(modem, "fc", 910);
  su_modem_set_float(modem, "rolloff", .35);

  if (!su_modem_start(modem)) {
    fprintf(stderr, "xsig_modem_init: failed to start modem\n");
    su_modem_destroy(modem);
    return NULL;
  }

  return modem;
}

SUPRIVATE void
xsigtool_redraw_channels(display_t *disp, const struct xsig_interface *iface)
{
  struct sigutils_channel **channel_list;
  unsigned int channel_count;
  unsigned int i;
  unsigned int a, b;
  unsigned int n = 0;
  SUFLOAT expand;
  unsigned int halfsize = iface->wf->params.fft_size / 2;

  su_channel_detector_get_channel_list(
      iface->cd,
      &channel_list,
      &channel_count);

  fbox(
      disp,
      iface->s->params.x,
      iface->s->params.y + iface->s->params.height + 2,
      iface->s->params.x + iface->s->params.width,
      iface->s->params.y + iface->s->params.height + 2 + 8 * 8,
      OPAQUE(0));

  expand = .5 * (SUFLOAT) iface->wf->params.width /
           (SUFLOAT) iface->wf->params.fft_size;
  for (i = 0; i < channel_count; ++i)
    if (channel_list[i] != NULL && SU_CHANNEL_IS_VALID(channel_list[i])) {
      a = expand * iface->wf->params.width
          * SU_ABS2NORM_FREQ(
              iface->cd->params.samp_rate,
              iface->cd->params.decimation
              * (channel_list[i]->fc - channel_list[i]->bw * .5));
      b = expand * iface->wf->params.width
          * SU_ABS2NORM_FREQ(
              iface->cd->params.samp_rate,
              iface->cd->params.decimation
              * (channel_list[i]->fc + channel_list[i]->bw * .5));

      a = (a + halfsize) % iface->wf->params.fft_size;
      b = (b + halfsize) % iface->wf->params.fft_size;

      fbox(
          disp,
          iface->s->params.x + a,
          iface->s->params.y + 1,
          iface->s->params.x + b,
          iface->s->params.y + iface->wf->params.height - 1,
          0x7fff0000);

      display_printf(
          disp,
          iface->s->params.x,
          iface->s->params.y + iface->s->params.height + 3 + n * 8,
          OPAQUE(0x7f7f7f),
          OPAQUE(0),
          "Channel %d: %lg Hz (bw: %lg Hz) / SNR: %lg dBFS",
          n,
          channel_list[i]->fc,
          channel_list[i]->bw,
          channel_list[i]->snr);
      ++n;
    }
}

int
main(int argc, char *argv[])
{
  su_modem_t *modem = NULL;
  struct xsig_constellation_params cons_params = xsig_constellation_params_INITIALIZER;
  struct xsig_waterfall_params wf_params;
  struct xsig_spectrum_params s_params;
  struct sigutils_channel_detector_params cd_params =
      sigutils_channel_detector_params_INITIALIZER;
  xsig_constellation_t *cons = NULL;
  textarea_t *area;
  display_t *disp;
  SUCOMPLEX sample = 0;
  unsigned int count = 0;
  struct xsig_source *instance;
  struct xsig_interface interface;
  SUFLOAT *fc;
  SUBOOL *abc;
  SUBOOL *afc;
  uint32_t colors[4] = {0xffffff7f, 0xff7f7fff, 0xff7fff7f, 0xffff7f7f};
  char sym;

  if (argc != 2) {
    fprintf(stderr, "Usage:\n\t%s file.wav\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if (!su_lib_init()) {
    fprintf(stderr, "%s: failed to initialize library\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if ((modem = xsig_modem_init(argv[1], &instance)) == NULL)
    exit(EXIT_FAILURE);

  if ((fc = su_modem_get_state_property_ref(
      modem,
      "fc",
      SU_PROPERTY_TYPE_FLOAT)) == NULL) {
    fprintf(stderr, "%s: failed to retrieve fc property\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if ((abc = su_modem_get_state_property_ref(
      modem,
      "abc",
      SU_PROPERTY_TYPE_BOOL)) == NULL) {
    fprintf(stderr, "%s: failed to retrieve abc property\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if ((afc = su_modem_get_state_property_ref(
      modem,
      "afc",
      SU_PROPERTY_TYPE_BOOL)) == NULL) {
    fprintf(stderr, "%s: failed to retrieve afc property\n", argv[0]);
    exit(EXIT_FAILURE);
  }


  if ((disp = display_new(SCREEN_WIDTH, SCREEN_HEIGHT)) == NULL) {
    fprintf(stderr, "%s: failed to initialize display\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  cons_params.scaling = .25;
  cons_params.history_size = 20;
  cons_params.width = 128;
  cons_params.height = 128;

  if ((cons = xsig_constellation_new(&cons_params)) == NULL) {
    fprintf(stderr, "%s: cannot create constellation\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  wf_params.fft_size = 512;
  wf_params.width = 512;
  wf_params.height = 128;
  wf_params.x = 3;
  wf_params.y = 133;

  if ((interface.wf = xsig_waterfall_new(&wf_params)) == NULL) {
    fprintf(stderr, "%s: cannot create waterfall\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  s_params.fft_size = 512;
  s_params.width = 512;
  s_params.height = 128;
  s_params.x = 3;
  s_params.y = 133 + wf_params.height + 4;

  /*
   * Scale factor is measured in height units / dB: This means that
   * 0 dBFS will be on top of the spectrum graph and -100 dBFS on
   * the bottom. Reference level of 0 dBFS can be adjusted with
   * the ref parameter.
   */
  s_params.scale = 1. / 128.;
  s_params.alpha = 5e-3;
  s_params.ref = 0; /* Value in dBFS of the top level of the spectrum graph */

  if ((interface.s = xsig_spectrum_new(&s_params)) == NULL) {
    fprintf(stderr, "%s: cannot create spectrum\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  cd_params.samp_rate = instance->samp_rate;
  cd_params.alpha = 1e-3;

  if ((interface.cd = su_channel_detector_new(&cd_params)) == NULL) {
    fprintf(stderr, "%s: cannot create channel detector\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  if ((area = display_textarea_new (disp, 133, 3, 48, 16, NULL, 850, 8))
      == NULL) {
    fprintf(stderr, "%s: failed to create textarea\n", argv[0]);
    exit(EXIT_FAILURE);
  }

  area->autorefresh = 0;

  instance->params.private = &interface;

  while (!isnan(SU_C_ABS(sample = su_modem_read_sample(modem)))) {
    sym = ((SU_C_REAL(sample) > 0) << 1) | (SU_C_IMAG(sample) > 0);
    xsig_constellation_feed(cons, sample);
    if (++count % cons_params.history_size == 0) {
      xsig_constellation_redraw(cons, disp);
      xsig_waterfall_redraw(interface.wf, disp);
      xsig_spectrum_redraw(interface.s, disp);
      xsigtool_redraw_channels(disp, &interface);
      display_printf(
          disp,
          2,
          disp->height - 9,
          OPAQUE(0xbfbfbf),
          OPAQUE(0),
          "Carrier: %8.3lf Hz", *fc);
      textarea_set_fore_color(area, colors[sym]);
      cprintf(area, "%c", sym + 'A');
      display_refresh(disp);
    }
    usleep(1000);
  }

  display_end(disp);

  su_modem_destroy(modem);

  return 0;
}
