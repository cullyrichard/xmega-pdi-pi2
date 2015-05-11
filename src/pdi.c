#include "pdi.h"
#include <sched.h>
#include <sys/mman.h>
#include <string.h>
#include <bcm2835.h>

typedef struct
{
  uint8_t val;
  enum {
    XF_ST = -1,
    XF_0, XF_1, XF_2, XF_3, XF_4, XF_5, XF_6, XF_7,
    XF_PAR, XF_SP0, XF_SP1
  } pos;
} byte_xfer_t;

static struct
{
  // pdi_run loop breaker
  volatile bool stop;

  // pins
  uint8_t clk;
  uint8_t data;

  // job
  pdi_sequence_done_fn_t done_fn;
  pdi_sequence_t *seq;

  // xfer tracking
  bool cur_failed;
  pdi_sequence_t *cur;
  uint32_t cur_offs;
  byte_xfer_t byte;

  bool switch_dir;
} pdi;



static void load_next_byte ()
{
  // if in input mode, store last received byte
  if (pdi.cur->xfer->dir == PDI_IN)
    pdi.cur->xfer->buf[pdi.cur_offs] = pdi.byte.val;

  if (++pdi.cur_offs >= pdi.cur->xfer->len)
  {
    bool old_dir = pdi.cur->xfer->dir;

    pdi.cur = pdi.cur->next;
    pdi.cur_offs = 0;

    if (pdi.cur && pdi.cur->xfer->dir != old_dir)
      pdi.switch_dir = true;
  }
  // reinit (also used if pdi.cur->xfer->dir == PDI_IN)
  pdi.byte.pos = XF_ST;
  if (pdi.cur && pdi.cur->xfer->dir == PDI_OUT)
    pdi.byte.val = (uint8_t)pdi.cur->xfer->buf[pdi.cur_offs];
  else
    pdi.byte.val = 0;
}


// https://graphics.stanford.edu/~seander/bithacks.html#ParityParallel
static bool parity (uint8_t v)
{
  v ^= v >> 4;
  v &= 0xf;
  return (0x6996 >> v) & 1;
}


static void clock_falling_edge (void)
{
  // TODO: wait for right moment to give clk edge
    bcm2835_delayMicroseconds (2);
  bcm2835_gpio_clr (pdi.clk);
}


static void clock_rising_edge (void)
{
  // TODO: wait for right moment to give clk edge
    bcm2835_delayMicroseconds (2);
  bcm2835_gpio_set (pdi.clk);
}


static void blind_clock (unsigned n)
{
  while (n--)
  {
    clock_falling_edge ();
    clock_rising_edge ();
  }
}


static void clock_out (void)
{
  clock_falling_edge ();
  if (!pdi.seq)
    bcm2835_gpio_set (pdi.data); // IDLE
  else
  {
    bool bit = 0;
    switch (pdi.byte.pos++)
    {
      case XF_ST: bit = 0; break;
      case XF_0: case XF_1: case XF_2: case XF_3: // fall-through
      case XF_4: case XF_5: case XF_6: case XF_7:
        bit = (pdi.byte.val >> (pdi.byte.pos -1)) & 1; break;
      case XF_PAR: bit = parity (pdi.byte.val); break;
      case XF_SP0: bit = 1; break;
      case XF_SP1: bit = 1; load_next_byte (); break;
    }
    if (bit)
      bcm2835_gpio_set (pdi.data);
    else
      bcm2835_gpio_clr (pdi.data);
  }
  clock_rising_edge ();
}


static void clock_in (void)
{
  clock_falling_edge ();
  clock_rising_edge ();
  if (pdi.seq)
  {
    bool bit = (bcm2835_gpio_lev (pdi.data) > 0);
    switch (pdi.byte.pos)
    {
      case XF_ST: pdi.byte.pos += !bit; break; // expect data next if low bit
      case XF_0: case XF_1: case XF_2: case XF_3:
      case XF_4: case XF_5: case XF_6: case XF_7:
        pdi.byte.val |= (bit << pdi.byte.pos); break;
      case XF_PAR:
        if (bit != parity (pdi.byte.val)) pdi.cur_failed = true; break;
      case XF_SP0: if (!bit) pdi.cur_failed = true; break;
      case XF_SP1: if (!bit) pdi.cur_failed = true; load_next_byte (); break;
    }
    if (pdi.byte.pos != XF_ST)
      ++pdi.byte.pos;
  }
}


static void report_done (void)
{
  pdi_sequence_done_fn_t done = pdi.done_fn;
  pdi_sequence_t *seq = pdi.seq;
  pdi.done_fn = 0;
  pdi.seq = pdi.cur = 0;
  done (pdi.cur_failed == false, seq);
}


// ----- Interface functions --------------------------------------------

bool pdi_init (uint8_t clk_pin, uint8_t data_pin)
{
  if (!bcm2835_init ())
    return false;

  pdi.stop = false;
  pdi.clk = clk_pin;
  pdi.data = data_pin;

  struct sched_param sp;
  memset (&sp, 0, sizeof(sp));
  sp.sched_priority = sched_get_priority_max (SCHED_FIFO);
  sched_setscheduler (0, SCHED_FIFO, &sp);
  mlockall (MCL_CURRENT | MCL_FUTURE);

  bcm2835_gpio_fsel (pdi.clk, BCM2835_GPIO_FSEL_OUTP);
  bcm2835_gpio_fsel (pdi.data, BCM2835_GPIO_FSEL_OUTP);
  bcm2835_gpio_clr (pdi.clk); // hold in reset until pdi_run()
  bcm2835_gpio_clr (pdi.data);

  return true;
}


bool pdi_set_sequence (pdi_sequence_t *seq, pdi_sequence_done_fn_t fn)
{
  if (pdi.seq || pdi.done_fn)
    return false;

  pdi.done_fn = fn;
  pdi.seq = pdi.cur = seq;
  pdi.cur_failed = false;
  pdi.cur_offs = 0;
  pdi.byte.pos = XF_ST;
  if (seq->xfer->dir == PDI_IN)
    pdi.byte.val = 0;
  else
    pdi.byte.val = (uint8_t)seq->xfer->buf[0];

  pdi.switch_dir = true; // ensure we do the right thing next

  return true;
}


void pdi_run (void)
{
  // put device into PDI mode
  bcm2835_gpio_set (pdi.data);
  bcm2835_delayMicroseconds (1); // xmega256a3 says 90-1000ns reset pulse width
  blind_clock (16); // next, 16 pdi_clk cycles within 100us

  // and now, clocketiclock...
  while (!pdi.stop && pdi.seq)
  {
    if (pdi.switch_dir)
    {
      if (pdi.cur->xfer->dir == PDI_OUT)
        bcm2835_gpio_fsel (pdi.data, BCM2835_GPIO_FSEL_OUTP);
      else
        bcm2835_gpio_fsel (pdi.data, BCM2835_GPIO_FSEL_INPT);
      pdi.switch_dir = false;
    }

    if (!pdi.seq || pdi.cur->xfer->dir == PDI_OUT)
      clock_out ();
    else
      clock_in ();

    if ((pdi.seq && !pdi.cur) || pdi.cur_failed) // just finished, or failed
      report_done ();
  }

  if (pdi.stop && pdi.done_fn)
  {
    pdi.cur_failed = true;
    report_done ();
  }
}


bool pdi_break (void)
{
  if (pdi.seq || pdi.done_fn)
    return false;

  bcm2835_gpio_fsel (pdi.data, BCM2835_GPIO_FSEL_OUTP);
  blind_clock (12);
  blind_clock (12);
  return true;
}


void pdi_stop (void)
{
  pdi.stop = true;
}