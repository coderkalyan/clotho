output "server_id" {
  value       = latitudesh_server.bench.id
  description = "Latitude server id — run.sh passes it to the in-box watchdog for self-delete."
}

output "ipv4" {
  value       = latitudesh_server.bench.primary_ipv4
  description = "Public IPv4 for SSH/rsync."
}
