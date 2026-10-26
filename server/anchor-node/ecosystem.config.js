// pm2 process file: `pm2 start ecosystem.config.js` in this folder. Settings (port, TLS, keys ...) are in config.json, not here.
module.exports = {
  apps: [
    {
      name: 'wwanchor',
      script: 'src/server.js',
      cwd: __dirname,
      interpreter: 'node',
      env: { NODE_ENV: 'production' },
      autorestart: true,
      min_uptime: '10s',          // a start that dies sooner than this counts as a crash loop ...
      max_restarts: 20,           // ... and pm2 gives up after this many in a row
      restart_delay: 3000,
      max_memory_restart: '1G',
      kill_timeout: 5000,
      time: true,                 // timestamps in pm2's own log view
      merge_logs: true,
      out_file: 'logs/out.log',
      error_file: 'logs/error.log',
    },
  ],
};
