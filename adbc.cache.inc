<?php

/**
 * @file
 *   Extends Drupal's default database cache.
 */

/**
 * Alternative implementation of Drupal's default cache backend.
 *
 * This cache backend extends Drupal's default database cache implementation by
 * altering how cached items are cleared. The primary goal is to increaser cache
 * hits by ensuring cron doesn't truncate complete bins.
 *
 * Also note that all comments have been stripped except those pertaining to
 * modifications here. See the original cache.inc file for more details.
 *
 * @see http://drupal.org/node/891600
 */
class AlternativeDrupalDatabaseCache extends DrupalDatabaseCache { 
  /**
   * Implements DrupalCacheInterface::getMultiple().
   *
   * We handle garbage collection differently; this method is reimplemented only
   * to remove a call to the garbageCollection() method.
   */
  function getMultiple(&$cids) {
    try {
      $result = db_query('SELECT cid, data, created, expire, serialized FROM {' . db_escape_table($this->bin) . '} WHERE cid IN (:cids)', array(':cids' => $cids));
      $cache = array();
      foreach ($result as $item) {
        $item = $this->prepareItem($item);
        if ($item) {
          $cache[$item->cid] = $item;
        }
      }
      $cids = array_diff($cids, array_keys($cache));
      return $cache;
    }
    catch (Exception $e) {
      return array();
    }
  }

  /**
   * Generic garbage collection method.
   *
   * Here, we're redefining a lot of our garbage collection logic, allowing bins
   * to be cleaned up on an entry-by-entry basis, rather than at a bin-level.
   *
   * Additionally, we've moved session cache handling to a separate method.
   */
  protected function garbageCollection() {
    if ($cache_lifetime = variable_get('cache_lifetime', 0)) {
      // Clear expired items from the cache.
      db_delete($this->bin)
        ->condition('expire', CACHE_PERMANENT, '>')
        ->condition('expire', REQUEST_TIME, '<')
        ->execute();

      // Clear CACHE_TEMPORARY items that are older than cache_lifetime.
      db_delete($this->bin)
        ->condition('expire', CACHE_TEMPORARY)
        ->condition('created', REQUEST_TIME - $cache_lifetime, '<')
        ->execute();
    }
    else {
      // No minimum cache lifetime. Flush all expired and temporary cache
      // entries. This is limited to a configurable frequency on cron.
      $backtrace = debug_backtrace();
      if ($backtrace[3]['function'] == 'system_cron') {
        $name = 'cache_garbage_collect_' . $this->bin;
        $window = variable_get('cache_garbage_collection_frequency', 86400);
        if (!flood_is_allowed($name, 1, $window, 'cron')) {
          return;
        }
        else {
          flood_register_event($name, $window, 'cron');
        }
      }
      db_delete($this->bin)
        ->condition('expire', CACHE_PERMANENT, '<>')
        ->condition('expire', REQUEST_TIME, '<')
        ->execute();
    }
  }

  /**
   * Cleans up the per-session cache expiration data.
   */
  protected function cleanSession() {
    $cache_lifetime = variable_get('cache_lifetime', 0);
    if (isset($_SESSION['cache_expiration'])) {
      $expire = REQUEST_TIME - $cache_lifetime;
      foreach ($_SESSION['cache_expiration'] as $bin => $timestamp) {
        if ($timestamp < $expire) {
          unset($_SESSION['cache_expiration'][$bin]);
        }
      }
      if (!$_SESSION['cache_expiration']) {
        unset($_SESSION['cache_expiration']);
      }
    }
  }

  /**
   * Implements DrupalCacheInterface::set().
   */
  function set($cid, $data, $expire = CACHE_PERMANENT) {
    // When setting cache items, clean up old session data in case it is stale.
    $this->cleanSession();

    return parent::set($cid, $data, $expire);
  }

  /**
   * Implements DrupalCacheInterface::clear().
   *
   * Rather than re-defining our garbage collection logic, we call the method
   * directly from within the clear method.
   */
  function clear($cid = NULL, $wildcard = FALSE) {
    if (empty($cid)) {
      $this->garbageCollection();
    }
    else {
      if ($wildcard) {
        if ($cid == '*') {
          db_truncate($this->bin)->execute();
        }
        else {
          db_delete($this->bin)
            ->condition('cid', db_like($cid) . '%', 'LIKE')
            ->execute();
        }
      }
      elseif (is_array($cid)) {
        do {
          db_delete($this->bin)
            ->condition('cid', array_splice($cid, 0, 1000), 'IN')
            ->execute();
        }
        while (count($cid));
      }
      else {
        db_delete($this->bin)
          ->condition('cid', $cid)
          ->execute();
      }
    }
  }
}
