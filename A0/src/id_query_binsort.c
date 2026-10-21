#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>

#include "record.h"
#include "id_query.h"


struct index_record{
  int64_t osm_id;
  const struct record *record;
};
struct indexed_data{
  struct index_record *irs;
  int n;
};

// function to compare two integers in ascending order
int ascending_compare(const void* a, const void* b){

    int64_t x = ((const struct index_record*)a)->osm_id;
    int64_t y = ((const struct index_record*)b)->osm_id;
    if (x < y) {
    return -1;
  } else if (x == y) {
    return 0;
  } else {
    return 1;
  }
}

struct indexed_data* mk_indexed(struct record* rs, int n){
  struct indexed_data* data = malloc(sizeof(*data));
  data->irs = malloc (n *sizeof(*data->irs));
  data->n = n;

  for (int i=0; i<n;i++){
    data->irs[i].record = &rs[i];
    data->irs[i].osm_id = rs[i].osm_id;
  }
    qsort(data->irs, n, sizeof(*data->irs),ascending_compare);
  return data;
}
void free_indexed(struct indexed_data* data){

  free(data->irs);
  free(data);
}
const struct record* lookup_binsort(struct indexed_data *data, int64_t needle){
  int low = 0;
  int high = data->n-1;
  while (low<=high){
    int mid =(low+high)/2;
    int64_t id = data->irs[mid].osm_id;
    if (id == needle){
      return data->irs[mid].record;
    } else if (id < needle){
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  return NULL;
}

int main(int argc, char** argv) {
  return id_query_loop(argc, argv,
                    (mk_index_fn)mk_indexed,
                    (free_index_fn)free_indexed,
                    (lookup_fn)lookup_binsort);
}
