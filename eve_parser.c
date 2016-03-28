#include "eve_parser.h"

#define E_JDAY 719558       /* Julian Day of epoch (1970-1-1) */
#define SEC_PER_DAY 86400
#define SEC_PER_HOUR 3600
#define SEC_PER_MIN 60

/* Fast converter between years and Epoch normalized Julian Days, in seconds */
typedef unsigned int uint;
static uint32_t ejday(const uint year, const uint month, const uint day)
{
	return (year*365 + year/4 - year/100 + year/400
	    + (month * 306 + 5) / 10 + day - 1 - E_JDAY) * SEC_PER_DAY;
}

/* Converts a pacific timestamp to UTC. Faster than mktime() by a lot. */
static uint32_t pt_to_utc(const uint32_t pacificTime)
{
	/* Attempt to switch PT to UTC (including daylight savings time. :\) */
	/* YYYY-MM-DD-HH in PT (These are the date-times where daylight savings
	* time apparently switches (according to tzdump on my machine) within
	* the range of dates that we care about. */
	#define D2006040203 1144033200
	#define D2006102901 1162256400
	#define D2007031103 1173754800
	#define D2007110400 1194307200

	if (pacificTime < D2006040203) {
		return pacificTime + 8 * SEC_PER_HOUR;
	} else if (pacificTime < D2006102901) {
		return pacificTime + 7 * SEC_PER_HOUR;
	} else if (pacificTime < D2007031103) {
		return pacificTime + 8 * SEC_PER_HOUR;
	} else {
		/* Techincally this should be an else-if, but since this time
		* period covers the remaining window where the historical data
		* is reported in PT (rather than UTC later) it doesn't matter
		*/
		return pacificTime + 7 * SEC_PER_HOUR;
	}
}

/* Returns -2 on failure. */
static int8_t range_to_byte(const int range)
{
	switch(range) {
	case -1:
		return -1;
	case 0:
		return 0;
	case 5:
		return 5;
	case 10:
		return 10;
	case 20:
		return 20;
	case 40:
		return 40;
	case 32767:
	case 65535:
		return 127;
	default:
		return -2;
	}
}

static uint64_t parse_uint64(const char **s)
{
	const char *str = *s;
	uint64_t val = 0;

	while (!isdigit(*str) && *str != '\0')
		str++; /* Skip leading non-digits */

	while (isdigit(*str))
		val = val * 10 + *str++ - '0'; /* Assume base 10 */

	*s = str;

	return val;
}

static uint32_t parse_uint32(const char **s)
{
	const char *str = *s;
	uint32_t val = 0;

	while (!isdigit(*str) && *str != '\0')
		str++; /* Skip leading non-digits */

	while (isdigit(*str))
		val = val * 10 + *str++ - '0'; /* Assume base 10 */

	*s = str;

	return val;
}

static int parse_int(const char **s)
{
	const char *str = *s;
	int val = 0;

	while (!isdigit(*str) && *str != '\0')
		str++; /* Skip leading non-digits */

	while (isdigit(*str))
		val = val * 10 + *str++ - '0'; /* Assume base 10 */

	*s = str;

	return val;
}

static int8_t parse_range(const char **s)
{
	const char *str = *s;

	while (!isdigit(*str) && *str != '\0' && *str != '-')
		str++; /* Skip leading non-digits */

	if (*str == '-') {
		return -1;
	}

	*s = str;
	return range_to_byte(parse_int(s));
}

/*
 * Return 0 on successful parsing, -1 on malformed input and -2 on alloc error.
 * 1 is returned for improper data.
 *
*/
/* It's "abstraction violating" but I'm not bothering to check for
 * memory allocation in set_token because I know that small datasets
 * < 9 bytes are copied to internal buffers and therefore there's no
 * memory allocation occuring to fail anyway.
*/
/* Input Order:
 * orderid, regionid, systemid, stationid, typeid, bid, price, volmin,
 * volrem, volent, issued, duration, range, reportedby, rtime
*/
void parser(const char *str, struct raw_record *rec)
{
	rec->orderID = parse_uint64(&str);
	rec->regionID = parse_uint32(&str);
	rec->systemID = parse_uint32(&str);
	rec->stationID = parse_uint32(&str);
	rec->typeID = parse_uint32(&str);
	rec->bid = (uint8_t)parse_int(&str);
	rec->price = parse_uint64(&str) * 100;
	if (*str == '.') { /* Cents & cent field are optional */
		str++;
		rec->price += (*str++ - '0') * 10;
		if (isdigit(*str)) {
			rec->price += (*str++ - '0');
		}
	}
	rec->volMin = parse_uint32(&str);
	rec->volRem = parse_uint32(&str);
	rec->volEnt = parse_uint32(&str);

	/* Year, month, day */
	rec->issued = ejday(parse_int(&str), parse_int(&str), parse_int(&str));

	rec->issued += parse_int(&str) * SEC_PER_HOUR;
	rec->issued += parse_int(&str) * SEC_PER_MIN;
	rec->issued += parse_int(&str);

	if (isdigit(*str) && *str - '0' > 5) {
		rec->issued++; /* Round time to the nearest second. */
	}

	rec->duration = (uint16_t)parse_uint32(&str); /* Day(s) */
	/* There's an hour, min, and sec field that's never used, so skip. */
	parse_int(&str);				/* Hour */
	parse_int(&str);				/* Min */
	parse_int(&str);				/* Sec  */

	/* Special since range has negatives. */
	rec->range = parse_range(&str);

	rec->reportedby = parse_uint64(&str);

	/* Year, month, day again */
	rec->rtime = ejday(parse_int(&str), parse_int(&str), parse_int(&str));

	rec->rtime += parse_int(&str) * SEC_PER_HOUR;
	rec->rtime += parse_int(&str) * SEC_PER_MIN;
	rec->rtime += parse_int(&str);

	if (isdigit(*str) && *str - '0' > 5) {
		rec->rtime++; /* Round time to the nearest second. */
	}
}

/*
 * Historical caveats: Buy order ranges incorrect, and time in Pacific.
 * There's no specifier in C89 for int8_t, so we get some compiler warnings
 * with %u. In C99 we'd use %hhu.
 *
*/
int parse_pt_bo(const char *str, struct raw_record *rec)
{
	parser(str, rec);

	/* Buy order ranges are incorrect for this period. */
	/* If it's a buy order (bid) then estimate the range as the smallest.*/
	if (rec->bid > 1) {
		return 1;
	} else if (rec->bid == 1) {
		rec->range = range_to_byte(-1);
	}

	/* If range is bad or issued is earlier than reported return bad val */
	if (rec->range == -2 || (rec->issued > rec->rtime)) {
		return 1;
	}

	/* Convert pacific time stamps to UTC. */
	rec->issued = pt_to_utc(rec->issued);
	rec->rtime = pt_to_utc(rec->rtime);

	return 0;
}

/* Historical caveat: Time in Pacific. (Buy order ranges now correct) */
int parse_pt(const char *str, struct raw_record *rec)
{
	parser(str, rec);

	/* If range is bad or issued is earlier than reported return bad val */
	if (rec->range == -2 || (rec->issued > rec->rtime)) {
		return 1;
	}

	/* Convert pacific time stamps to UTC. */
	rec->issued = pt_to_utc(rec->issued);
	rec->rtime = pt_to_utc(rec->rtime);

	return 0;
}

/* Historical caveat: Switch to UTC. */
int parse(const char *str, struct raw_record *rec)
{
	parser(str, rec);

	/* If range is bad or issued is earlier than reported return bad val */
	if (rec->range == -2 || (rec->issued > rec->rtime)) {
		return 1;
	}

	return 0;
}

Parser parser_factory(const uint32_t yr, const uint32_t mn, const uint32_t dy)
{
	#define D20070101 1167609600
	#define D20071001 1191369600
	#define D20100718 1279584000
	#define D20110213 1297468800

	const uint64_t parsedTime = ejday(yr, mn, dy);

	if (parsedTime < D20070101) {
		return parse_pt_bo;
	} else if (parsedTime < D20071001) {
		return parse_pt;
	} else {
		return parse;
	}
}
