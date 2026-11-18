FROM postgres:17

# The official image's entrypoint still does the first-run setup (initdb and the scripts in
# /docker-entrypoint-initdb.d). node-entrypoint.sh decides beforehand whether this node starts
# as the primary or as a standby copied from its peer, and afterwards watches for failovers.
COPY database/node-entrypoint.sh /usr/local/bin/node-entrypoint.sh
COPY database/init/ /docker-entrypoint-initdb.d/
# an init script without the execute bit would be sourced into the entrypoint's own shell
RUN chmod 755 /usr/local/bin/node-entrypoint.sh /docker-entrypoint-initdb.d/*.sh

ENTRYPOINT ["node-entrypoint.sh"]
