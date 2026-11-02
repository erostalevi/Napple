output "instance_id" {
  description = "EC2 instance the deploy rolls through SSM (and `aws ssm start-session --target` for a shell)."
  value       = aws_instance.servers.id
}

output "public_ip" {
  description = "Elastic IP of the servers (handed to napple-online as NAPPLE_ONLINE_PUBLIC_IP for NEX station URLs)."
  value       = aws_eip.servers.public_ip
}

output "server_fqdn" {
  value = local.server_fqdn
}

output "ecr_repository_urls" {
  description = "Image repositories by name."
  value       = { for k, r in aws_ecr_repository.image : k => r.repository_url }
}

output "log_group" {
  value = aws_cloudwatch_log_group.servers.name
}

output "region" {
  value = var.region
}

output "zone_name_servers" {
  description = "The zone's name servers: the registrar of napple.lat must list exactly these."
  value       = data.aws_route53_zone.main.name_servers
}
