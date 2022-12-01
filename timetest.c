
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>

void cal_date(struct timespec *tp){
	int microsecond = tp->tv_nsec;
	int days = tp->tv_sec / (3600 * 24);
	int hours = (tp->tv_sec % (3600 * 24)) / 3600;
	int minutes = (tp->tv_sec % 3600) / 60;
	int seconds = (tp->tv_sec % 3600) % 60;
	int year = 0;
	int last_year = 0;
	int total_count_days = 0; 
	int leap_count = 2;
	while(total_count_days < days){
		last_year = total_count_days;
		if (leap_count != 4){
			total_count_days += 365;
			leap_count++;
		}else{
			total_count_days += 366;
			leap_count = 1;
		}
		if (total_count_days <= days){
			year++;
		}
	}
	int days_left = days - last_year;
	int month = 1;
	int last_month = 0;
	while (days_left > 0){
		last_month = days_left;
		if (month == 1 || month == 3 || month == 5 || month == 7 || month == 8 || month == 10 || month == 12){
			days_left -= 31;
		}else if (month == 4 || month == 6 || month == 9 || month == 11){
			days_left -= 30;
		}else if (month == 2 && leap_count == 1){
			days_left -= 29;
		}else{
			days_left -= 28;
		}
		if (days_left >= 0){
			month++;
		}
	}
    if (hours - 4 < 0){
        last_month -= 1;
        hours -= 4;
        hours += 24;
    }else{
        hours -= 4;
    }
    last_month++;
	printf("%d-%d-%d %d:%d:%d:%d\n",(1970 + year), month, last_month, hours, minutes, seconds, microsecond);

}

int main(int argc, char *argv[]){
    struct timespec sp;
    clock_gettime(CLOCK_REALTIME, &sp);
    cal_date(&sp);
    return 0;
}