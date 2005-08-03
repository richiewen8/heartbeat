/* $Id: iso8601.c,v 1.3 2005/08/03 12:55:13 andrew Exp $ */
/* 
 * Copyright (C) 2005 Andrew Beekhof <andrew@beekhof.net>
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 * 
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * http://en.wikipedia.org/wiki/ISO_8601 as at 2005-08-01
 *
 */

#include <crm/crm.h>
#include <time.h>
#include <ctype.h>
#include <crm/common/iso8601.h>

gboolean gregorian_to_ordinal(ha_time_t *a_date);
gboolean ordinal_to_gregorian(ha_time_t *a_date);
gboolean ordinal_to_weekdays(ha_time_t *a_date);
void normalize_time(ha_time_t *a_time);


void
log_date(int log_level, const char *prefix, ha_time_t *date_time, int flags) 
{
	char *date_s = NULL;
	char *time_s = NULL;
	char *offset_s = NULL;
	ha_time_t *dt = NULL;
	
	if(flags & ha_log_local) {
		crm_debug_6("Local version");
		dt = date_time;
	} else {
		dt = date_time->normalized;
	}

	CRM_DEV_ASSERT(dt != NULL); if(crm_assert_failed) { return; }
	
	if(flags & ha_log_date) {
		crm_malloc0(date_s, sizeof(char)*(32));
		if(date_s == NULL) {
		} else if(flags & ha_date_weeks) {
			snprintf(date_s, 31, "%d-W%.2d-%d",
				 dt->weekyears, dt->weeks, dt->weekdays);

		} else if(flags & ha_date_ordinal) {
			snprintf(date_s, 31, "%d-%.3d",dt->years, dt->yeardays);

		} else {
			snprintf(date_s, 31, "%.4d-%.2d-%.2d",
				 dt->years, dt->months, dt->days);
		}
	}
	if(flags & ha_log_time) {
		int offset = 0;
		crm_malloc0(time_s, sizeof(char)*(32));
		if(time_s == NULL) {
			return;
		} 

		snprintf(time_s, 31, "%.2d:%.2d:%.2d",
			 dt->hours, dt->minutes, dt->seconds);
		
		if(dt->offset != NULL) {
			offset =(dt->offset->hours * 100) + dt->offset->minutes;
		}
		
		crm_malloc0(offset_s, sizeof(char)*(32));
		if((flags & ha_log_local) == 0 || offset == 0) {
			snprintf(offset_s, 31, "Z");

		} else {
			snprintf(offset_s, 31, " %s%.2d:%.2d",
				 offset>0?"+":"-",
				 dt->offset->hours, dt->offset->minutes);
		}
	}
	crm_log_maybe(log_level, "%s%s%s%s%s%s",
		      prefix?prefix:"", prefix?": ":"",
		      date_s?date_s:"", (date_s!=NULL&&time_s!=NULL)?" ":"",
		      time_s?time_s:"", offset_s?offset_s:"");

	crm_free(date_s);
	crm_free(time_s);
	crm_free(offset_s);
}

void
log_time_period(int log_level, ha_time_period_t *dtp, int flags) 
{
	log_date(log_level, "Period start:", dtp->start, flags);
	log_date(log_level, "Period end:", dtp->end, flags);
}

ha_time_t*
parse_time_offset(char **offset_str)
{
	ha_time_t *new_time = NULL;
	crm_malloc0(new_time, sizeof(ha_time_t));
	crm_malloc0(new_time->has, sizeof(ha_has_time_t));

	if((*offset_str)[0] != 'Z') {
		parse_time(offset_str, new_time, FALSE);
	}
	return new_time;
}

ha_time_t*
parse_time(char **time_str, ha_time_t *a_time, gboolean with_offset)
{
	ha_time_t *new_time = a_time;
	if(a_time == NULL) {
		new_time = new_ha_date(FALSE);
	}

	CRM_DEV_ASSERT(new_time != NULL);
	CRM_DEV_ASSERT(new_time->has != NULL);

	crm_debug_4("Get hours...");
	if(parse_int(time_str, 2, 24, &new_time->hours)) {
		new_time->has->hours = TRUE;
	}

	crm_debug_4("Get minutes...");
	if(parse_int(time_str, 2, 60, &new_time->minutes)) {
		new_time->has->minutes = TRUE;
	}

	crm_debug_4("Get seconds...");
	if(parse_int(time_str, 2, 60, &new_time->seconds)){
		new_time->has->seconds = TRUE;
	}
	   
	if(with_offset) {
		crm_debug_4("Get offset...");
		while(isspace((*time_str)[0])) {
			(*time_str)++;
		}

		new_time->offset = parse_time_offset(time_str);
	}
	return new_time;
}

void
normalize_time(ha_time_t *a_time)
{
	CRM_DEV_ASSERT(a_time != NULL);
	CRM_DEV_ASSERT(a_time->has != NULL);

	if(a_time->normalized == NULL) {
		crm_malloc0(a_time->normalized, sizeof(ha_time_t));
	}
	if(a_time->normalized->has == NULL) {
		crm_malloc0(a_time->normalized->has, sizeof(ha_has_time_t));
	}

	ha_set_time(a_time->normalized, a_time, FALSE);
	if(a_time->offset != NULL) {
		if(a_time->offset->has->hours) {
			add_hours(a_time->normalized, a_time->offset->hours);
		}
		if(a_time->offset->has->minutes) {
			add_minutes(a_time->normalized,a_time->offset->minutes);
		}
		if(a_time->offset->has->seconds) {
			add_seconds(a_time->normalized,a_time->offset->seconds);
		}
	}
	CRM_DEV_ASSERT(is_date_sane(a_time));
}



ha_time_t *
parse_date(char **date_str)
{
	gboolean converted = FALSE;
	ha_time_t *new_time = NULL;
	crm_malloc0(new_time, sizeof(ha_time_t));
	crm_malloc0(new_time->has, sizeof(ha_has_time_t));

	CRM_DEV_ASSERT(date_str != NULL);
	CRM_DEV_ASSERT(strlen(*date_str) > 0);
	
	while(isspace((*date_str)[0]) == FALSE) {
		char ch = (*date_str)[0];
		crm_debug_5("Switching on ch=%c (len=%d)",
			    ch, (int)strlen(*date_str));
		
		if(ch == 0) {
			/* all done */
			break;

		} else if(ch == '/') {
			/* all done - interval marker */
			break;
			
		} else if(ch == 'W') {
			CRM_DEV_ASSERT(new_time->has->weeks == FALSE);
			(*date_str)++;
			if(parse_int(date_str, 2, 53, &new_time->weeks)){
				new_time->has->weeks = TRUE;
				new_time->weekyears  = new_time->years;
				new_time->has->weekyears = new_time->has->years;
			}
			if((*date_str)[0] == '-') {
				(*date_str)++;
				if(parse_int(date_str, 1, 7, &new_time->weekdays)) {
					new_time->has->weekdays = TRUE;
				}
			}
			
			if(new_time->weekdays == 0
			   || new_time->has->weekdays == FALSE) {
				new_time->weekdays = 1;
				new_time->has->weekdays = TRUE;
			}
			
		} else if(ch == '-') {
			(*date_str)++;
			if(check_for_ordinal(*date_str)) {
				if(parse_int(date_str, 3, 366, &new_time->yeardays)) {
					new_time->has->yeardays = TRUE;
				}
			}
			
		} else if(ch == 'O') {
			/* ordinal date */
			(*date_str)++;
			if(parse_int(date_str, 3, 366, &new_time->yeardays)){
				new_time->has->yeardays = TRUE;
			}

		} else if(ch == 'T') {
			if(new_time->has->yeardays) {
				converted = convert_from_ordinal(new_time);
				
			} else if(new_time->has->weekdays) {
				converted = convert_from_weekdays(new_time);
				
			} else {
				converted = convert_from_gregorian(new_time);
			}
			(*date_str)++;
			parse_time(date_str, new_time, TRUE);

		} else if(isdigit(ch)) {
			if(new_time->has->years == FALSE
			   && parse_int(date_str, 4, 9999, &new_time->years)) {
				new_time->has->years = TRUE;
				
			} else if(check_for_ordinal(*date_str) && parse_int(
					  date_str, 3,
					  is_leap_year(new_time->years)?366:365,
					  &new_time->yeardays)) {
				new_time->has->yeardays = TRUE;
				
			} else if(new_time->has->months == FALSE
				  && parse_int(date_str, 2, 12, &new_time->months)) {
					new_time->has->months = TRUE;
				
			} else if(new_time->has->days == FALSE) {
				if(parse_int(date_str, 2,
					     days_per_month(new_time->months, new_time->years),
					     &new_time->days)) {
					new_time->has->days = TRUE;
				}
			}

		} else if(isspace(ch)) {
			(*date_str)++;
/* 		} else if(new_time->has->months == FALSE) { */
/* 			new_time->months = str_lookup(*date_str, date_month); */
/* 			new_time->has->months = TRUE; */
			
/* 		} else if(new_time->has->days == FALSE) { */
/* 			new_time->days = str_lookup(*date_str, date_day); */
/* 			new_time->has->days = TRUE; */
		} else {
			crm_err("Unexpected characters at: %s", *date_str);
			break;
		}
	}

	if(converted) {
		
	} else if(new_time->has->yeardays) {
		convert_from_ordinal(new_time);

	} else if(new_time->has->weekdays) {
		convert_from_weekdays(new_time);

	} else {
 		convert_from_gregorian(new_time);
	}

	normalize_time(new_time);
	
	log_date(LOG_DEBUG_3, "Unpacked", new_time, ha_log_date|ha_log_time);

	CRM_DEV_ASSERT(is_date_sane(new_time));
	
	return new_time;
}

ha_time_t*
parse_time_duration(char **interval_str)
{
	gboolean is_time = FALSE;
	ha_time_t *diff = NULL;
	crm_malloc0(diff, sizeof(ha_time_t));
	crm_malloc0(diff->has, sizeof(ha_has_time_t));

	CRM_DEV_ASSERT(interval_str != NULL);
	CRM_DEV_ASSERT(strlen(*interval_str) > 0);
	
	CRM_DEV_ASSERT((*interval_str)[0] == 'P');
	(*interval_str)++;
	
	while(isspace((*interval_str)[0]) == FALSE) {
		int an_int = 0;
		char ch = 0;

		if((*interval_str)[0] == 'T') {
			is_time = TRUE;
			(*interval_str)++;
		}
		
		if(parse_int(interval_str, 10, 0, &an_int) == FALSE) {
			break;
		}
		ch = (*interval_str)[0];
		(*interval_str)++;

		crm_debug_4("%c=%d", ch, an_int);
		
		switch(ch) {
			case 0:
				return diff;
				break;
			case 'Y':
				diff->years = an_int;
				diff->has->years = TRUE;
				break;
			case 'M':
				if(is_time) {
					diff->minutes = an_int;
					diff->has->minutes = TRUE;
				} else {
					diff->months = an_int;
					diff->has->months = TRUE;
				}
				break;
			case 'W':
				diff->weeks = an_int;
				diff->has->weeks = TRUE;
				break;
			case 'D':
				diff->days = an_int;
				diff->has->days = TRUE;
				break;
			case 'H':
				diff->hours = an_int;
				diff->has->hours = TRUE;
				break;
			case 'S':
				diff->seconds = an_int;
				diff->has->seconds = TRUE;
				break;
			default:
				break;
		}
	}
	return diff;
}

ha_time_period_t*
parse_time_period(char **period_str)
{
	gboolean invalid = FALSE;
	const char *original = *period_str;
	ha_time_period_t *period = NULL;
	crm_malloc0(period, sizeof(ha_time_period_t));

	CRM_DEV_ASSERT(period_str != NULL);
	CRM_DEV_ASSERT(strlen(*period_str) > 0);

	tzset();
	
	if((*period_str)[0] == 'P') {
		period->diff = parse_time_duration(period_str);
	} else {
		period->start = parse_date(period_str);
	}

	if((*period_str)[0] != 0) {
		CRM_DEV_ASSERT((*period_str)[0] == '/');
		(*period_str)++;
		
		if((*period_str)[0] == 'P') {
			period->diff = parse_time_duration(period_str);
		} else {
			period->end = parse_date(period_str);
		}
		
	} else if(period->diff != NULL) {
		/* just aduration starting from now */
		time_t now = time(NULL);
		crm_malloc0(period->start, sizeof(ha_time_t));
		crm_malloc0(period->start->has, sizeof(ha_has_time_t));
		crm_malloc0(period->start->offset, sizeof(ha_time_t));
		crm_malloc0(period->start->offset->has, sizeof(ha_has_time_t));
		
		ha_set_timet_time(period->start, &now);
		normalize_time(period->start);
	} else {
		CRM_DEV_ASSERT((*period_str)[0] == '/');
		return NULL;
	}
	
	
	/* sanity checks */
	if(period->start == NULL && period->end == NULL) {
		crm_err("Invalid time period: %s", original);
		invalid = TRUE;
		
	} else if(period->start == NULL && period->diff == NULL) {
		crm_err("Invalid time period: %s", original);
		invalid = TRUE;

	} else if(period->end == NULL && period->diff == NULL) {
		crm_err("Invalid time period: %s", original);
		invalid = TRUE;
	}

	if(invalid) {
		crm_free(period->start);
		crm_free(period->end);
		crm_free(period->diff);
		crm_free(period);
		return NULL;
	}
	if(period->end == NULL && period->diff == NULL) {
	}
	
	if(period->start == NULL) {
		period->start = subtract_time(period->end, period->diff);
		normalize_time(period->start);
		
	} else if(period->end == NULL) {
		period->end = add_time(period->start, period->diff);
		normalize_time(period->end);
	}

	is_date_sane(period->start);
	is_date_sane(period->end);
	
	return period;
}

int month2days[13] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365 };

/* http://www.personal.ecu.edu/mccartyr/ISOwdALG.txt */
int
january1(int year) 
{
	int YY = (year - 1) % 100;
	int C = (year - 1) - YY;
	int G = YY + YY/4;
	int jan1 = 1 + (((((C / 100) % 4) * 5) + G) % 7);
	crm_debug_6("YY=%d, C=%d, G=%d", YY, C, G);
	crm_debug_5("January 1 %.4d: %d", year, jan1);
	return jan1;
}

int
weeks_in_year(int year) 
{
	int weeks = 52;
	int jan1 = january1(year);
	/* if jan1 == thursday */
	if(jan1 == 4) {
		weeks++;
	} else {
		jan1 = january1(year+1);
		/* if dec31 == thursday aka. jan1 of next year is a friday */
		if(jan1 == 5) {
			weeks++;
		}
		
	}
	return weeks;
}

gboolean
convert_from_gregorian(ha_time_t *a_date)
{
	CRM_DEV_ASSERT(gregorian_to_ordinal(a_date));
	CRM_DEV_ASSERT(ordinal_to_weekdays(a_date));
	return TRUE;
}

gboolean
gregorian_to_ordinal(ha_time_t *a_date)
{
	CRM_DEV_ASSERT(a_date->has->years);
	CRM_DEV_ASSERT(a_date->has->months);
	CRM_DEV_ASSERT(a_date->has->days);

	CRM_DEV_ASSERT(a_date->months > 0);
	CRM_DEV_ASSERT(a_date->days > 0);

	a_date->yeardays = month2days[a_date->months-1];
	a_date->yeardays += a_date->days;
	a_date->has->yeardays = TRUE;
	
	if(is_leap_year(a_date->years) && a_date->months > 2) {
		(a_date->yeardays)++;
	}
	crm_debug_4("Converted %.4d-%.2d-%.2d to %.4d-%.3d",
		    a_date->years, a_date->months, a_date->days,
		    a_date->years, a_date->yeardays);
	
	return TRUE;
}

gboolean
convert_from_ordinal(ha_time_t *a_date)
{
	CRM_DEV_ASSERT(ordinal_to_gregorian(a_date));
	CRM_DEV_ASSERT(ordinal_to_weekdays(a_date));
	return TRUE;
}


gboolean ordinal_to_gregorian(ha_time_t *a_date) 
{
	CRM_DEV_ASSERT(a_date->has->years);
	CRM_DEV_ASSERT(a_date->has->yeardays);

	CRM_DEV_ASSERT(a_date->yeardays > 0);
	
	a_date->days = a_date->yeardays;
	a_date->months = 11;
	if(is_leap_year(a_date->years) && a_date->yeardays > 366) {
		crm_err("Year %.4d only has 366 days (supplied %.3d)",
			a_date->years, a_date->yeardays);
		a_date->yeardays = 366;
		
	} else if(!is_leap_year(a_date->years) && a_date->yeardays > 365) {
		crm_err("Year %.4d only has 365 days (supplied %.3d)",
			a_date->years, a_date->yeardays);
		a_date->yeardays = 365;
	}
	
	while(a_date->months > 0
	      && a_date->yeardays <= month2days[a_date->months]) {
		crm_debug_6("month %d: %d vs. %d",
			    a_date->months, a_date->yeardays,
			    month2days[a_date->months]);
		(a_date->months)--;
	}

	a_date->days -= month2days[a_date->months];
	(a_date->months)++;
	
	CRM_DEV_ASSERT(a_date->months > 0);

	if(is_leap_year(a_date->years) && a_date->months > 2) {
		(a_date->days)--;
	}
	if(a_date->days == 0) {
		/* annoying underflow */
		a_date->days = days_per_month(a_date->months, a_date->years);
		(a_date->months)--;
	}

	a_date->has->days = TRUE;
	a_date->has->months = TRUE;
	a_date->has->years = TRUE;

	crm_debug_4("Converted %.4d-%.3d to %.4d-%.2d-%.2d",
		    a_date->years, a_date->yeardays,
		    a_date->years, a_date->months, a_date->days);
	
	return TRUE;
}


gboolean
ordinal_to_weekdays(ha_time_t *a_date)
{
	int year_num = 0;
	int jan1 = january1(a_date->years);
	int h = -1;

	CRM_DEV_ASSERT(a_date->has->years);
	CRM_DEV_ASSERT(a_date->has->yeardays);
	CRM_DEV_ASSERT(a_date->yeardays > 0);
	
	h = a_date->yeardays + jan1 - 1;
 	a_date->weekdays = 1 + ((h-1) % 7);
	a_date->has->weekdays = TRUE;

	if(a_date->yeardays <= (8-jan1) && jan1 > 4) {
		year_num = a_date->years - 1;
		a_date->weeks = weeks_in_year(year_num);
		a_date->has->weeks = TRUE;
		
	} else {
		year_num = a_date->years;
	}

	if(year_num == a_date->years) {
		int i = 365;
		if(is_leap_year(year_num)) {
			i = 366;
		}
		if( (i - a_date->yeardays) < (4 - a_date->weekdays) ) {
			year_num = a_date->years + 1;
			a_date->weeks = 1;
			a_date->has->weeks = TRUE;
		}
	}

	if(year_num == a_date->years) {
		int j = a_date->yeardays + (7-a_date->weekdays) + (jan1 - 1);
		a_date->weeks = j / 7;
		a_date->has->weeks = TRUE;
		if(jan1 > 4) {
			a_date->weeks -= 1;
		}
	}

	a_date->weekyears = year_num;
	a_date->has->weekyears = TRUE;
	crm_debug_4("Converted %.4d-%.3d to %.4dW%.2d-%d",
		    a_date->years, a_date->yeardays,
		    a_date->weekyears, a_date->weeks, a_date->weekdays);
	return TRUE;
}

gboolean
convert_from_weekdays(ha_time_t *a_date)
{
	gboolean conversion = FALSE;
	int jan1 = january1(a_date->weekyears);

	CRM_DEV_ASSERT(a_date->has->weekyears);
	CRM_DEV_ASSERT(a_date->has->weeks);
	CRM_DEV_ASSERT(a_date->has->weekdays);

	CRM_DEV_ASSERT(a_date->weeks > 0);
	CRM_DEV_ASSERT(a_date->weekdays > 0);
	CRM_DEV_ASSERT(a_date->weekdays < 8);
	
	a_date->has->years = TRUE;
	a_date->years = a_date->weekyears;

	a_date->has->yeardays = TRUE;
	a_date->yeardays = (7 * (a_date->weeks-1));

	/* break up the addition to make sure overflows are correctly handled */
	if(a_date->yeardays == 0) {
		a_date->yeardays = a_date->weekdays;
	} else {
		add_yeardays(a_date, a_date->weekdays);
	}
	
	crm_debug_5("Pre-conversion: %dW%d-%d to %.4d-%.3d",
		    a_date->weekyears, a_date->weeks, a_date->weekdays,
		    a_date->years, a_date->yeardays);

	conversion = ordinal_to_gregorian(a_date);

	if(conversion) {
		if(jan1 < 4) {
			sub_days(a_date, jan1-1);
		} else if(jan1 > 4) {
			add_days(a_date, jan1-4);
		}
	}
	return conversion; 
}

void
ha_set_time(ha_time_t *lhs, ha_time_t *rhs, gboolean offset)
{
	crm_debug_6("lhs=%p, rhs=%p, offset=%d", lhs, rhs, offset);

	CRM_DEV_ASSERT(lhs != NULL && rhs != NULL);
	CRM_DEV_ASSERT(lhs->has != NULL && rhs->has != NULL);
	
	lhs->years = rhs->years;
	lhs->has->years = rhs->has->years;

	lhs->weekyears = rhs->weekyears;
	lhs->has->weekyears = rhs->has->weekyears;
	
	lhs->months = rhs->months;
	lhs->has->months = rhs->has->months;

	lhs->weeks = rhs->weeks;
	lhs->has->weeks = rhs->has->weeks;

	lhs->days = rhs->days;
	lhs->has->days = rhs->has->days;

	lhs->weekdays = rhs->weekdays;
	lhs->has->weekdays = rhs->has->weekdays;
	
	lhs->yeardays = rhs->yeardays;
	lhs->has->yeardays = rhs->has->yeardays;

	lhs->hours = rhs->hours;
	lhs->has->hours = rhs->has->hours;

	lhs->minutes = rhs->minutes;
	lhs->has->minutes = rhs->has->minutes;

	lhs->seconds = rhs->seconds;
	lhs->has->seconds = rhs->has->seconds;

	if(lhs->offset) {
		reset_time(lhs->offset);
	}
	if(offset && rhs->offset) {
		ha_set_time(lhs->offset, rhs->offset, FALSE);
	}
	
}



void
ha_set_tm_time(ha_time_t *lhs, struct tm *rhs)
{
	int wday = rhs->tm_wday;
	int h_offset = 0;
	int m_offset = 0;
	
	if(rhs->tm_year > 0) {
		/* years since 1900 */
		lhs->years = 1900 + rhs->tm_year;
		lhs->has->years = TRUE;
	}

	if(rhs->tm_yday > 0) {
		/* days since January 1 [0-365] */
		lhs->yeardays = 1 + rhs->tm_yday;
		lhs->has->yeardays =TRUE;
	}

	if(rhs->tm_hour >= 0) {
		lhs->hours = rhs->tm_hour;
		lhs->has->hours =TRUE;
	}
	if(rhs->tm_min >= 0) {
		lhs->minutes = rhs->tm_min;
		lhs->has->minutes =TRUE;
	}
	if(rhs->tm_sec >= 0) {
		lhs->seconds = rhs->tm_sec;
		lhs->has->seconds =TRUE;
	}

	convert_from_ordinal(lhs);

	/* months since January [0-11] */
	CRM_DEV_ASSERT(rhs->tm_mon < 0  || lhs->months == (1 + rhs->tm_mon));

	/* day of the month [1-31] */
	CRM_DEV_ASSERT(rhs->tm_mday < 0 || lhs->days == rhs->tm_mday);

	/* days since Sunday [0-6] */
	if(wday == 0) {
		wday= 7;
	}
	CRM_DEV_ASSERT(rhs->tm_wday < 0 || lhs->weekdays == wday);
	
	CRM_DEV_ASSERT(lhs->offset != NULL);
	CRM_DEV_ASSERT(lhs->offset->has != NULL);

	/* tm_gmtoff == offset from UTC in seconds */
	h_offset = rhs->tm_gmtoff / (3600); 
	m_offset = (rhs->tm_gmtoff - (3600 * h_offset)) / (60);
	crm_debug_6("Offset (s): %ld, offset (hh:mm): %.2d:%.2d",
		    rhs->tm_gmtoff, h_offset, m_offset);
	
	lhs->offset->hours = h_offset;
	lhs->offset->has->hours = TRUE;

	lhs->offset->minutes = m_offset;
	lhs->offset->has->minutes = TRUE;

	normalize_time(lhs);
}


void
ha_set_timet_time(ha_time_t *lhs, time_t *rhs)
{
	ha_set_tm_time(lhs, localtime(rhs));
}

ha_time_t *
add_time(ha_time_t *lhs, ha_time_t *rhs)
{
	ha_time_t *answer = NULL;
	CRM_DEV_ASSERT(lhs != NULL && rhs != NULL);
	
	answer = new_ha_date(FALSE);
	ha_set_time(answer, lhs, TRUE);	

	normalize_time(lhs);
	normalize_time(answer);

	if(rhs->has->years) {
		add_years(answer, rhs->years);
	}
	if(rhs->has->months) {
		add_months(answer, rhs->months);
	}
	if(rhs->has->weeks) {
		add_weeks(answer, rhs->weeks);
	}
	if(rhs->has->days) {
		add_days(answer, rhs->days);
	}

	add_hours(answer, rhs->hours);
	add_minutes(answer, rhs->minutes);
	add_seconds(answer, rhs->seconds);

	return answer;
}


ha_time_t *
subtract_time(ha_time_t *lhs, ha_time_t *rhs)
{
	ha_time_t *answer = NULL;
	CRM_DEV_ASSERT(lhs != NULL && rhs != NULL);

	answer = new_ha_date(FALSE);
	ha_set_time(answer, lhs, TRUE);	

	normalize_time(lhs);
	normalize_time(rhs);
	normalize_time(answer);

	sub_years(answer, rhs->years);
	sub_months(answer, rhs->months);
	sub_weeks(answer, rhs->weeks);
	sub_days(answer, rhs->days);
	sub_hours(answer, rhs->hours);
	sub_minutes(answer, rhs->minutes);
	sub_seconds(answer, rhs->seconds);
}

/* ha_time_interval_t* */
/* parse_time_interval(char **interval_str) */
/* { */
/* 	return NULL; */
/* } */

int month_days[13] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31, 29 };
int
days_per_month(int month, int year)
{
	month--;
	if(month == 2 && is_leap_year(year)) {
		month = 13;
	}
	return month_days[month];
}

gboolean
is_leap_year(int year) 
{
	gboolean is_leap = FALSE;
	if(year % 4 == 0) {
		is_leap = TRUE;
	}
	if(year % 100 == 0 && year % 400 != 0 ) {
		is_leap = FALSE;
	}
	return is_leap;
}


gboolean
parse_int(char **str, int field_width, int uppper_bound, int *result)
{
	int lpc = 0;
	int intermediate = 0;
	gboolean fraction = FALSE;
	gboolean negate = FALSE;

	CRM_DEV_ASSERT(str != NULL);
	CRM_DEV_ASSERT(*str != NULL);
	CRM_DEV_ASSERT(result != NULL);

	*result = 0;

	if(strlen(*str) <= 0) {
		return FALSE;
	}
	
	crm_debug_6("max width: %d, first char: %c", field_width, (*str)[0]);
	
	if((*str)[0] == '.' || (*str)[0] == ',') {
		fraction = TRUE;
		field_width = -1;
		(*str)++;
	} else if((*str)[0] == '-') {
		negate = TRUE;
		(*str)++;
	} else if((*str)[0] == '+'
		|| (*str)[0] == ':') {
		(*str)++;
	}

	for(; (fraction || lpc < field_width) && isdigit((*str)[0]); lpc++) {
		if(fraction) {
			intermediate = ((*str)[0] - '0')/(10^lpc);
		} else {
			*result *= 10;
			intermediate = (*str)[0] - '0';
		}
		*result += intermediate;
		(*str)++;
	}
	if(fraction) {
		*result = (int)(*result * uppper_bound);
		
	} else if(uppper_bound > 0 && *result > uppper_bound) {
		*result = uppper_bound;
	}
	if(negate) {
		*result = 0 - *result;
	}
	if(lpc > 0) {
		crm_debug_5("Found int: %d", *result);
		return TRUE;
	}
	return FALSE;
}

gboolean
check_for_ordinal(const char *str)
{
	if(isdigit(str[2]) == FALSE) {
		crm_debug_6("char 3 == %c", str[2]);
		return FALSE;		
	}
	if(isspace(str[3])) {
		return TRUE;
	} else if(str[3] == 0) {
		return TRUE;
	} else if(str[3] == 'T') {
		return TRUE;
	} else if(str[3] == '/') {
		return TRUE;
	}
	crm_debug_6("char 4 == %c", str[3]);
	return FALSE;
}

int str_lookup(const char *str, enum date_fields field)
{
	return 0;
}

void
reset_time(ha_time_t *a_time)
{
	a_time->years = 0;
	a_time->has->years = FALSE;
		
	a_time->weekyears = 0;
	a_time->has->weekyears = FALSE;
		
	a_time->months = 0;
	a_time->has->months = FALSE;
		
	a_time->weeks = 0;
	a_time->has->weeks = FALSE;
		
	a_time->days = 0;
	a_time->has->days = FALSE;
		
	a_time->weekdays = 0;
	a_time->has->weekdays = FALSE;
		
	a_time->yeardays = 0;
	a_time->has->yeardays = FALSE;
		
	a_time->hours = 0;
	a_time->has->hours = FALSE;
		
	a_time->minutes = 0;
	a_time->has->minutes = FALSE;
		
	a_time->seconds = 0;
	a_time->has->seconds = FALSE;
}


void
reset_tm(struct tm *some_tm) 
{
	some_tm->tm_sec = -1;	 /* seconds after the minute [0-60] */
	some_tm->tm_min = -1;	 /* minutes after the hour [0-59] */
	some_tm->tm_hour = -1;	 /* hours since midnight [0-23] */
	some_tm->tm_mday = -1;	 /* day of the month [1-31] */
	some_tm->tm_mon = -1;	 /* months since January [0-11] */
	some_tm->tm_year = -1;	 /* years since 1900 */
	some_tm->tm_wday = -1;	 /* days since Sunday [0-6] */
	some_tm->tm_yday = -1;	 /* days since January 1 [0-365] */
	some_tm->tm_isdst = -1;	 /* Daylight Savings Time flag */
	some_tm->tm_gmtoff = -1; /* offset from CUT in seconds */
	some_tm->tm_zone = NULL;/* timezone abbreviation */
}

gboolean
is_date_sane(ha_time_t *a_date)
{
	int ydays = 0;
	int mdays = 0;
	int weeks = 0;

	CRM_DEV_ASSERT(a_date != NULL);
	
	ydays = is_leap_year(a_date->years)?366:365;
	mdays = days_per_month(a_date->months, a_date->years);
	weeks = weeks_in_year(a_date->weekyears);
	crm_debug_5("max ydays: %d, max mdays: %d, max weeks: %d",
		    ydays, mdays, weeks);
	
	CRM_DEV_ASSERT(a_date->has->years);
	CRM_DEV_ASSERT(a_date->has->weekyears);

	CRM_DEV_ASSERT(a_date->has->months);
	CRM_DEV_ASSERT(a_date->months > 0);
	CRM_DEV_ASSERT(a_date->months < 13);

	CRM_DEV_ASSERT(a_date->has->weeks);
	CRM_DEV_ASSERT(a_date->weeks > 0);
	CRM_DEV_ASSERT(a_date->weeks < weeks);

	CRM_DEV_ASSERT(a_date->has->days);
	CRM_DEV_ASSERT(a_date->days > 0);
	CRM_DEV_ASSERT(a_date->days <= mdays);

	CRM_DEV_ASSERT(a_date->has->weekdays);
	CRM_DEV_ASSERT(a_date->weekdays > 0);
	CRM_DEV_ASSERT(a_date->weekdays < 8);

	CRM_DEV_ASSERT(a_date->has->yeardays);
	CRM_DEV_ASSERT(a_date->yeardays > 0);
	CRM_DEV_ASSERT(a_date->yeardays <= ydays);

	CRM_DEV_ASSERT(a_date->hours >= 0);
	CRM_DEV_ASSERT(a_date->hours < 24);

	CRM_DEV_ASSERT(a_date->minutes >= 0);
	CRM_DEV_ASSERT(a_date->minutes < 60);
	
	CRM_DEV_ASSERT(a_date->seconds >= 0);
	CRM_DEV_ASSERT(a_date->seconds < 60);
	
	return TRUE;
}

#define do_cmp_field(lhs, rhs, field)					\
	{								\
		if(lhs->field > rhs->field) {				\
			crm_debug_2("%s: %d > %d",			\
				    #field, lhs->field, rhs->field);	\
			return 1;					\
		} else if(lhs->field < rhs->field) {			\
			crm_debug_2("%s: %d < %d",			\
				    #field, lhs->field, rhs->field);	\
			return -1;					\
		}							\
	}

int
compare_date(ha_time_t *lhs, ha_time_t *rhs)
{
	normalize_time(lhs);
	normalize_time(rhs);

	do_cmp_field(lhs->normalized, rhs->normalized, years);
	do_cmp_field(lhs->normalized, rhs->normalized, yeardays);
	do_cmp_field(lhs->normalized, rhs->normalized, hours);
	do_cmp_field(lhs->normalized, rhs->normalized, minutes);
	do_cmp_field(lhs->normalized, rhs->normalized, seconds);

	return 0;
}

ha_time_t *
new_ha_date(gboolean set_to_now) 
{
	time_t tm_now = time(NULL);
	ha_time_t *now = NULL;
	crm_malloc0(now, sizeof(ha_time_t));
	crm_malloc0(now->has, sizeof(ha_has_time_t));
	crm_malloc0(now->offset, sizeof(ha_time_t));
	crm_malloc0(now->offset->has, sizeof(ha_has_time_t));
	if(set_to_now) {
		ha_set_timet_time(now, &tm_now);
	}
	return now;
}

void
free_ha_date(ha_time_t *a_date) 
{
	if(a_date == NULL) {
		return;
	}
	free_ha_date(a_date->normalized);
	free_ha_date(a_date->offset);
	crm_free(a_date->has);
	crm_free(a_date);
}
