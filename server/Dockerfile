# Live Collab relay server. Build:  docker build -t collab-server server
# Run:                              docker run -d -p 5588:5588 -v collab-data:/data collab-server
FROM node:20-alpine
WORKDIR /app
COPY server.js ./
ENV PORT=5588 DATA_DIR=/data
VOLUME /data
EXPOSE 5588
CMD ["node", "server.js"]
