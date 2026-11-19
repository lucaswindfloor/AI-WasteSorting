function key(view, kind) {
  return 'm08:' + [view.station, view.run, view.session, kind].map(encodeURIComponent).join(':');
}

export const savedIds = {
  remember(view, id, kind = 'operation') {
    try {
      localStorage.setItem(key(view, kind), id);
    } catch {
      // Storage can be disabled by the browser. The operation still appears in the workbench.
    }
  },

  last(view, kind = 'operation') {
    try {
      return localStorage.getItem(key(view, kind));
    } catch {
      return null;
    }
  },
};
