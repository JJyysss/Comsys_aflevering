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

struct kd_node {
    const struct record *record;
    int axis;
    struct kd_node *left;
    struct kd_node *right;
};

struct kd_data {
    struct kd_node *root;
};

int compare_lon(const void *a, const void *b) {
  const struct record *ra = *(const struct record **)a;
  const struct record *rb = *(const struct record **)b;
  if (ra->lon < rb->lon) {
    return -1;
  } else if (ra->lon > rb->lon) {
    return 1;
  } else {
    return 0;
  }
}

int compare_lat(const void *a, const void *b) {
  const struct record *ra = *(const struct record **)a;
  const struct record *rb = *(const struct record **)b;
  if (ra->lat < rb->lat) {
    return -1;
  } else if (ra->lat > rb->lat) {
    return 1;
  } else {
    return 0;
  }
}

struct kd_node *build_tree(const struct record **points, int n, int depth) {
    if (n==0) {
        return NULL;
    };
    int axis = depth % 2;
    if (axis==0) {
        qsort(points, n, sizeof(*points), compare_lon);
    } else {
        qsort(points, n, sizeof(*points), compare_lat);
    };
    int mid = n/2;
    struct kd_node *node = malloc(sizeof(*node));
    node->record = points[mid];
    node->axis = axis;
    node->left = build_tree(points, mid, depth + 1);
    node->right = build_tree(points + mid + 1, n - mid - 1, depth + 1);

    return node;
}

struct kd_data *mk_kdtree(const struct record *rs, int n) {
  struct kd_data *data = malloc(sizeof(*data));
  const struct record **points = malloc(n * sizeof(*points));
  for (int i = 0; i < n; i++) {
    points[i] = &rs[i];
  }
  data->root = build_tree(points, n, 0);
  free(points);
  return data;
}

double distance_sq(const struct record *r, double lon, double lat) {
  double dx = r->lon - lon;
  double dy = r->lat - lat;
  return dx * dx + dy * dy;
}

void lookup_recursive(
    struct kd_node *node, double lon, double lat,
    const struct record **best, double *best_dist) {
  if (node == NULL) {
    return;
  }
  double d = distance_sq(node->record, lon, lat);
  if (d < *best_dist) {
    *best_dist = d;
    *best = node->record;
  }
  double diff;
  if (node->axis == 0) {
    diff = node->record->lon - lon;
  } else {
    diff = node->record->lat - lat;
  }
  struct kd_node *near;
  struct kd_node *far;
  if (diff >= 0) {
    near = node->left;
    far = node->right;
  } else {
    near = node->right;
    far = node->left;
  }
  lookup_recursive(near, lon, lat, best, best_dist);
  if (diff * diff < *best_dist) {
    lookup_recursive(far, lon, lat, best, best_dist);
  }
}

const struct record *lookup_kdtree(
    struct kd_data *data, double lon, double lat) {
  const struct record *best = NULL;
  double best_dist = INFINITY;
  lookup_recursive(data->root, lon, lat, &best, &best_dist);
  return best;
}

void free_tree(struct kd_node *node) {
  if (node == NULL) { return; }
  free_tree(node->left);
  free_tree(node->right);
  free(node);
}

void free_kdtree(struct kd_data *data) {
  free_tree(data->root);
  free(data);
}

int main(int argc, char **argv) {
  return coord_query_loop(argc, argv,
      (mk_index_fn)mk_kdtree,
      (free_index_fn)free_kdtree,
      (lookup_fn)lookup_kdtree);
}
