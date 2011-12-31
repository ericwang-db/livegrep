var path = require('path'),
    fs   = require('fs');

var config = {
  DNODE_PORT: 0xC5EA,
  SEARCH_REPO: path.join(__dirname, "../../linux"),
  SEARCH_REF:  "v3.0",
  SEARCH_ARGS: []
};

try {
  fs.statSync(path.join(__dirname, 'config.local.js'));
  var local = require('./config.local.js');
  Object.keys(local).forEach(
    function (k){
      config[k] = local[k]
    })
} catch (e) {
}

module.exports = config;