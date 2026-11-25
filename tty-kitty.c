/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Michael Grant <mgrant@grant.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/* Detect support in the outside terminal and decode its key sequences. */

static int
tty_kitty_parse_number(const char *s, u_int *value, int empty)
{
	char		*end;
	unsigned long	 number;

	if (*s == '\0') {
		if (!empty)
			return (-1);
		return (0);
	}
	if (!isdigit((u_char)*s))
		return (-1);
	number = strtoul(s, &end, 10);
	if (*end != '\0' || number > UINT_MAX)
		return (-1);
	*value = number;
	return (0);
}

/* Parse a colon separated list, where an empty item is zero. */
static int
tty_kitty_parse_list(char *s, u_int *values, u_int maximum, u_int *count)
{
	char	*item;

	*count = 0;
	while ((item = strsep(&s, ":")) != NULL) {
		if (*count == maximum)
			return (-1);
		values[*count] = 0;
		if (tty_kitty_parse_number(item, &values[*count], 1) != 0)
			return (-1);
		(*count)++;
	}
	return (0);
}

static int
tty_kitty_find_end(const char *buf, size_t len, size_t *end)
{
	size_t	i;

	for (i = 2; i < len; i++) {
		if (buf[i] == 'u' || buf[i] == '~' || buf[i] == 'A' ||
		    buf[i] == 'B' || buf[i] == 'C' || buf[i] == 'D' ||
		    buf[i] == 'E' || buf[i] == 'F' || buf[i] == 'H' ||
		    buf[i] == 'P' || buf[i] == 'Q' || buf[i] == 'S') {
			*end = i;
			return (0);
		}
		if (!isdigit((u_char)buf[i]) && buf[i] != ';' && buf[i] != ':')
			return (-1);
	}
	return (1);
}

static key_code
tty_kitty_function(u_int number, char final)
{
	key_code	key;

	key = input_kitty_find(number, final);
	if (key != KEYC_NONE) {
		if (final >= 'A' && final <= 'D')
			key |= KEYC_CURSOR;
		return (key);
	}
	if (final == 'u')
		return (KEYC_NONE);

	/* Other forms terminals send for some keys. */
	if (final == '~') {
		switch (number) {
		case 7:
			return (KEYC_HOME);
		case 8:
			return (KEYC_END);
		case 11:
			return (KEYC_F1);
		case 12:
			return (KEYC_F2);
		case 14:
			return (KEYC_F4);
		case 29:
			return (KEYC_MENU);
		case 57427:
			return (KEYC_KP_BEGIN|KEYC_KEYPAD);
		}
	} else if (final == 'E' && number == 1)
		return (KEYC_KP_BEGIN|KEYC_KEYPAD);
	return (KEYC_UNKNOWN);
}

int
tty_keys_kitty_query(struct tty *tty, const char *buf, size_t len,
    size_t *size, int apply)
{
	struct client	*c = tty->client;
	size_t		 i;
	u_int		 flags = 0;

	*size = 0;
	if (tty->flags & TTY_HAVEKKB)
		return (-1);

	if (buf[0] != '\033')
		return (-1);
	if (len == 1)
		return (1);
	if (buf[1] != '[')
		return (-1);
	if (len == 2)
		return (1);
	if (buf[2] != '?')
		return (-1);
	if (len == 3)
		return (1);

	for (i = 3; i < len && isdigit((u_char)buf[i]); i++) {
		if (flags > (UINT_MAX - (buf[i] - '0')) / 10)
			return (-1);
		flags = (flags * 10) + (buf[i] - '0');
	}
	if (i == len)
		return (1);
	if (i == 3 || buf[i] != 'u')
		return (-1);

	*size = i + 1;
	if (!apply)
		return (0);

	tty->kitty_keys = flags;
	tty->flags |= TTY_HAVEKKB;
	log_debug("%s: received Kitty keyboard flags %u", c->name, flags);
	tty_parse_client_features(c, "kittykeys", ",");
	tty_update_features(tty);
	return (0);
}

void
tty_update_kitty(struct tty *tty, struct screen *s)
{
	u_int	flags;
	char	buf[32];

	if (options_get_number(global_options, "extended-keys") == 0 ||
	    (~tty->term->flags & TERM_KITTYKEYS)) {
		if (tty->flags & TTY_KKBPUSHED) {
			tty_puts(tty, "\033[<u");
			tty->flags &= ~TTY_KKBPUSHED;
		}
		return;
	}
	if (s == NULL && (tty->flags & TTY_KKBPUSHED))
		return;

	/*
	 * Ask for whatever the pane wants, plus shifted keys so Alt+Shift+1
	 * is M-!. Once events or all keys are wanted, ask for everything:
	 * every key then has its unshifted number and text, enough to encode
	 * it for any pane and to match each release with its press.
	 */
	flags = KITTY_KEY_DISAMBIGUATE|KITTY_KEY_REPORT_ALTERNATES;
	if (s != NULL)
		flags |= input_kitty_flags(s);
	if (flags & (KITTY_KEY_REPORT_EVENTS|KITTY_KEY_REPORT_ALL)) {
		flags |= KITTY_KEY_REPORT_ALTERNATES|KITTY_KEY_REPORT_ALL|
		    KITTY_KEY_REPORT_TEXT;
	}
	if ((tty->flags & TTY_KKBPUSHED) == 0) {
		xsnprintf(buf, sizeof buf, "\033[>%uu", flags);
		tty_puts(tty, buf);
		tty->flags |= TTY_KKBPUSHED;
	} else if (tty->kitty_keys != flags) {
		xsnprintf(buf, sizeof buf, "\033[=%uu", flags);
		tty_puts(tty, buf);
	}
	tty->kitty_keys = flags;
}

int
tty_keys_kitty(struct tty *tty, const char *buf, size_t len, size_t *size,
    key_code *key, struct kitty_key *kk)
{
	struct client		*c = tty->client;
	struct kitty_key	 nk;
	struct utf8_data	 ud;
	utf8_char		 uc;
	size_t			 end;
	char			 tmp[256], *copy, *field, final;
	u_int			 values[3], count, fields = 0, cp, keymods;
	key_code		 nkey, onlykey;
	int			 result;

	*size = 0;
	if ((tty->flags & TTY_KKBPUSHED) == 0)
		return (-1);
	if (len == 0 || buf[0] != '\033')
		return (-1);
	if (len == 1)
		return (1);
	if (buf[1] != '[')
		return (-1);
	if (len == 2)
		return (1);
	if (buf[2] == '?')
		return (-1);

	result = tty_kitty_find_end(buf, len, &end);
	if (result != 0)
		return (result);
	if (end - 2 >= sizeof tmp)
		return (-1);
	memcpy(tmp, buf + 2, end - 2);
	tmp[end - 2] = '\0';
	final = buf[end];
	*size = end + 1;

	memset(&nk, 0, sizeof nk);
	nk.final = final;
	nk.modifiers = 1;
	nk.event = KITTY_EVENT_PRESS;

	copy = tmp;
	while ((field = strsep(&copy, ";")) != NULL) {
		if (fields == 0) {
			if (*field == '\0') {
				if (final == 'u')
					return (-1);
				nk.number = 1;
			} else {
				if (tty_kitty_parse_list(field, values, 3,
				    &count) != 0)
					return (-1);
				nk.number = values[0];
				if (count > 1)
					nk.shifted = values[1];
				if (count > 2)
					nk.base = values[2];
			}
		} else if (fields == 1) {
			if (tty_kitty_parse_list(field, values, 2, &count) != 0)
				return (-1);
			if (count > 0 && *field != '\0')
				nk.modifiers = values[0];
			if (count > 1)
				nk.event = values[1];
		} else if (fields == 2) {
			if (tty_kitty_parse_list(field, nk.text,
			    KITTY_TEXT_MAX, &nk.ntext) != 0)
				return (-2);
		} else
			return (-1);
		fields++;
	}
	if (nk.number == 0 || nk.modifiers == 0)
		return (-1);
	if (nk.event < KITTY_EVENT_PRESS || nk.event > KITTY_EVENT_RELEASE)
		return (-2);
	nk.modifiers--;
	keymods = nk.modifiers;

	nkey = tty_kitty_function(nk.number, final);
	if (nkey == KEYC_NONE) {
		if (final != 'u')
			return (-1);
		if (nk.number >= 57344 && nk.number <= 63743)
			return (-2);

		/*
		 * A key with only Shift and Alt is the character it types,
		 * without the Shift, as with other terminals.
		 */
		cp = nk.number;
		if ((keymods & ~(KITTY_MOD_SHIFT|KITTY_MOD_ALT|KITTY_MOD_META|
		    KITTY_MOD_CAPSLOCK|KITTY_MOD_NUMLOCK)) == 0) {
			if (nk.ntext == 1 && nk.text[0] >= 0x20 &&
			    nk.text[0] != 0x7f)
				cp = nk.text[0];
			else if ((keymods & KITTY_MOD_SHIFT) && nk.shifted != 0)
				cp = nk.shifted;
			else if ((keymods & KITTY_MOD_SHIFT) &&
			    cp >= 'a' && cp <= 'z')
				cp -= 'a' - 'A';
			if (cp > 0x20 && cp != 0x7f)
				keymods &= ~KITTY_MOD_SHIFT;
		}
		if (cp <= 0x7f)
			nkey = cp;
		else if (utf8_fromwc(cp, &ud) != UTF8_DONE ||
		    utf8_from_data(&ud, &uc) != UTF8_DONE)
			return (-2);
		else
			nkey = uc;
	} else if (nkey == KEYC_UNKNOWN)
		return (-1);

	if (keymods & KITTY_MOD_SHIFT)
		nkey |= KEYC_SHIFT;
	/* Keypad keys have no sequence with Meta, so it is sent as ESC. */
	if (keymods & (KITTY_MOD_ALT|KITTY_MOD_META)) {
		nkey |= KEYC_META;
		if (~nkey & KEYC_KEYPAD)
			nkey |= KEYC_IMPLIED_META;
	}
	if (keymods & KITTY_MOD_CTRL)
		nkey |= KEYC_CTRL;
	if (keymods & KITTY_MOD_SUPER)
		nkey |= KEYC_SUPER;
	if (keymods & KITTY_MOD_HYPER)
		nkey |= KEYC_HYPER;

	/* Shift-Tab has a dedicated tmux key code. */
	onlykey = nkey & KEYC_MASK_KEY;
	if (onlykey == C0_HT && (nkey & KEYC_SHIFT))
		nkey = KEYC_BTAB|(nkey & ~KEYC_MASK_KEY & ~KEYC_SHIFT);

	if (log_get_level() != 0) {
		log_debug("%s: Kitty key %.*s is %llx (%s) event %u", c->name,
		    (int)*size, buf, nkey, key_string_lookup_key(nkey, 1),
		    nk.event);
	}
	*key = nkey;
	*kk = nk;
	return (0);
}
