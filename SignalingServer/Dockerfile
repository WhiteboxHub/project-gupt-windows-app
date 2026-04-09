# Use the official Node.js 20 image
FROM node:20-slim

# Create app directory
WORKDIR /usr/src/app

# Copy package files
COPY package*.json ./

# Install production dependencies
RUN npm install --only=production

# Copy server source
COPY server.js ./

# Expose the port (GCP Cloud Run injects PORT=8080 by default)
EXPOSE 8080

# Command to run the server
CMD [ "node", "server.js" ]
