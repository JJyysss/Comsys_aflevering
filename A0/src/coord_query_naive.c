#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <math.h>

#include "record.h"
#include "coord_query.h"

// lookup lon and lat by checking through the array of records in the naive_data struct

struct naive_data {
  struct record *rs;
  int n;
};

struct naive_data* mk_naive(struct record* rs, int n) {
  struct naive_data* data = malloc (sizeof(*data));
  data->rs = rs;
  data->n = n;
  return data;
}

void free_naive(struct naive_data* data) {
  free (data);

}

const struct record* lookup_naive(struct naive_data *data, double lon, double lat) {
  // we need to check the given lon and lat with the records in data->rs
  double closest = INFINITY;
  const struct record* closest_data = NULL;
  for (int i = 0; i<data->n; i++){
    double dx = data->rs[i].lon - lon;
    double dy = data->rs[i].lat - lat;
    double distance = dx * dx + dy * dy;
    if (closest>distance){
      closest = distance;
      closest_data = &data->rs[i];
    } 
  }
  return (closest_data);
}

int main(int argc, char** argv) {
  return coord_query_loop(argc, argv,
                          (mk_index_fn)mk_naive,
                          (free_index_fn)free_naive,
                          (lookup_fn)lookup_naive);
}

