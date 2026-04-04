# Url-Shortener
This project explores the implementation of a URL shortener using C++, boost library and caddy reverse proxy.

## Features
- Shorten long URLs into compact, easy-to-share links.
- Redirect users from the shortened URL to the original long URL.
- Store URL mappings in a simple in-memory data structure.


## API Endpoints

| Endpoint                  | Description                                      |
|---------------------------|--------------------------------------------------|
| `POST /api/shorten`       | Accepts a long URL and returns a shortened URL   |
| `GET /api/code/{code}`    | Redirects to the original long URL               |
| `GET /api/expand/{code}`  | Returns the original long URL for the given code |
| `GET /api/health`         | Provides a health check endpoint                 |


## Local Setup
For local development, you can run using Docker Compose. Ensure you have Docker and Docker Compose installed on your machine.

Run the following commands in your terminal:
```bash
cd deploy 
docker-compose -f docker-compose.yaml up
```

## Preview
Demo link: https://caddy-reverse-proxy-production-a008.up.railway.app/ 